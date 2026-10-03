<!-- markdownlint-disable MD013 MD060 -->
# PSNR

Peak Signal-to-Noise Ratio: the log-ratio of the maximum possible sample
value to the mean squared error between the reference and the distorted
frame. Higher is better, in dB; identical frames are `+inf` in theory and the
finite stand-in `psnr_max` in the output (see
[the ceiling](#the-psnr_max-ceiling-and-the-uncapped-option)).

Two extractors ship it:

- `psnr` is the fixed-point (integer-accumulated) path. VMAF model JSON files
  and the CLI's `--feature psnr` select it.
- `float_psnr` converts both planes to `float` first and is kept for parity
  with upstream consumers of the float pipeline. It emits a luma-only score.

## Extractors

| Registered name | Backend | Feature names | Source |
|---|---|---|---|
| `psnr` | CPU (+ AVX2 / AVX-512 / NEON) | `psnr_y`, `psnr_cb`, `psnr_cr` | `core/src/feature/integer_psnr.c` |
| `float_psnr` | CPU (+ AVX2 / AVX-512 / NEON) | `float_psnr` | `core/src/feature/float_psnr.c` |
| `psnr_cuda`, `psnr_sycl`, `psnr_hip`, `integer_psnr_metal` | GPU twins of `psnr` | same as `psnr` | `core/src/feature/{cuda,sycl,hip,metal}/integer_psnr_*` |
| `float_psnr_cuda`, `float_psnr_sycl`, `float_psnr_hip`, `float_psnr_metal` | GPU twins of `float_psnr` | `float_psnr` | `core/src/feature/{cuda,sycl,hip,metal}/float_psnr_*` |

GPU twins are selected automatically when the corresponding `--backend` is
active, and they emit the same feature names as the CPU extractor, so a model
JSON referencing `psnr` works unchanged.

## How to run

```bash
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr --feature float_psnr \
    --output /dev/stdout
```

Options follow the feature name after the first `=`, separated by `:`, for
example `--feature psnr=enable_mse=true:enable_apsnr=true`.

## Output

- **Metrics.** `psnr_y`, `psnr_cb`, `psnr_cr` (fixed); `float_psnr` (float),
  plus `mse_*` and `apsnr_*` when the corresponding option is on.
- **Range.** dB. The lower bound is unbounded in principle (a fully inverted
  frame at 8 bpc gives about 0 dB). The upper bound is `psnr_max` unless
  `uncapped` is set, in which case only the `mse == 0` case reports
  `psnr_max`.
- **Input formats.** YUV 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0 at 8 / 10 / 12 / 16
  bpc.

## How the score is computed

For each plane `p`:

```text
sse_p = sum over samples of (ref - dis)^2
mse_p = sse_p / (w_p * h_p)
psnr_p = 10 * log10(peak^2 / mse_p)
```

`peak` is `(1 << bpc) - 1` for the integer extractor (255 at 8 bpc, 1023 at
10 bpc, and so on). `float_psnr` normalises high bit depths back onto an
8-bit scale and uses `peak` = 255.0 / 255.75 / 255.9375 / 255.99609375 for
8 / 10 / 12 / 16 bpc.

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

### `float_psnr`

| Option | Type | Default | Effect |
|---|---|---|---|
| `uncapped` | bool | `false` | As above. |

`float_psnr` is luma-only on every backend and has no `min_sse`.

## The `psnr_max` ceiling and the `uncapped` option

When the two planes are byte-identical the SSE is zero and the true PSNR is
`+inf`, which no output schema can carry. Both extractors therefore report a
finite stand-in, `psnr_max`:

| Bit depth | `psnr` (`6 × bpc + 12`) | `float_psnr` |
|---|---|---|
| 8 | 60 dB | 60 dB |
| 10 | 72 dB | 72 dB |
| 12 | 84 dB | 84 dB |
| 16 | 108 dB | 108 dB |

The `uncapped` option (bool, default `false`, on `psnr`, `float_psnr` and all
eight GPU twins under the same name) drops the truncation and keeps the
sentinel ([ADR-1193](../adr/1193-psnr-uncapped-option.md)):

| Case | default | `uncapped=true` |
|---|---|---|
| `mse == 0` (identical planes) | `psnr_max` | `psnr_max` |
| `mse > 0`, true PSNR below `psnr_max` | true value | true value |
| `mse > 0`, true PSNR above `psnr_max` | `psnr_max` (**truncated**) | true value |

The default is bit-identical to previous releases, so `uncapped` never moves
an existing score unless you ask for it. It does not change any feature name.

```bash
# Truncated at the 60 dB ceiling (the default)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr --feature float_psnr --output /dev/stdout
#   "psnr_y": 60.000000   "float_psnr": 60.000000

# True value reported; identical chroma planes still report the 60 dB sentinel
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr=uncapped=true \
    --feature float_psnr=uncapped=true --output /dev/stdout
#   "psnr_y": 100.840479  "psnr_cb": 60.000000  "float_psnr": 100.840479
```

The pair in the example differs by a single luma step: SSE 1 over 186624
samples, true PSNR 100.840479 dB.

### When to use `uncapped`

- **Comparing against another PSNR implementation.** FFmpeg's `psnr` filter
  reports 100.840479 on the example pair.
- **Scoring near-lossless encodes.** Clipping at 60 dB destroys the ranking
  between candidates.
- **Leave it off** when you need scores comparable with previously published
  VMAF/PSNR numbers.

!!! note
    VMAF model JSON files consume the *capped* feature. Turning `uncapped` on
    changes what a model that includes a PSNR term sees.

### `min_sse`, the older escape hatch

`min_sse` (double, default `0.0`) constrains the minimum MSE, which raises
`psnr_max` to `ceil(10 * log10(peak^2 / (min_sse / n_samples)))`. It also
lifts the score of *identical* planes, because it moves the sentinel rather
than removing the truncation: on the pair above,
`--feature psnr=min_sse=0.000001` gives `psnr_y = 100.840479` but reports
`psnr_cb = 155.000000` for byte-identical chroma. Prefer `uncapped` unless
you specifically want a raised sentinel.

`min_sse` belongs to the integer `psnr` extractor only; of its GPU twins,
`psnr_cuda`, `psnr_sycl` and `psnr_hip` implement it.

## GPU twins

The twins are listed under [Extractors](#extractors). The CUDA, SYCL and HIP
twins of both extractors return the CPU's bits; only Metal is held to a
tolerance (see [Backends](../backends/index.md)).

### Exactness

| Twin | Exact vs CPU | ADR | Evidence (fragment) |
|---|---|---|---|
| `psnr_cuda` | Yes | [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md), ADR-1457 | `scripts/ci/exact_twins.d/psnr.cuda` |
| `psnr_sycl` | Yes | [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), ADR-1451 | `scripts/ci/exact_twins.d/psnr.sycl` |
| `psnr_hip` | Yes | [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md), ADR-1437 | `scripts/ci/exact_twins.d/psnr.hip` |
| `float_psnr_cuda` | Yes | [ADR-1455](../adr/1455-cuda-float-psnr-exact-block-sums.md), [ADR-1499](../adr/1499-float-psnr-twins-cpu-row-order.md) | `scripts/ci/exact_twins.d/float_psnr.cuda` |
| `float_psnr_sycl` | Yes | [ADR-1450](../adr/1450-sycl-float-psnr-exact-block-sums.md), ADR-1499 | `scripts/ci/exact_twins.d/float_psnr.sycl` |
| `float_psnr_hip` | Yes | [ADR-1440](../adr/1440-hip-float-psnr-exact-block-sums.md), ADR-1499 | `scripts/ci/exact_twins.d/float_psnr.hip` |
| `integer_psnr_metal`, `float_psnr_metal` | Tolerance (`FEATURE_TOLERANCE`) | none | none |

### Option support of the `psnr` twins

| Twin | Options implemented | `--subsample` | Notes |
|---|---|---|---|
| `psnr_cuda` | All six | Sees every frame, so `apsnr_*` covers the whole clip | Since 2026-09-30 |
| `psnr_sycl` | All six | Does not yet see every frame | Matches `--backend cpu` bit for bit with every option set |
| `psnr_hip` | All six | Sees every frame | Since 2026-09-30; on a gfx1036 its `psnr_*`, `mse_*` and `apsnr_*` equal the CPU's on the Netflix 576x324 pair with every option set |
| `integer_psnr_metal` | `enable_chroma`, `uncapped` only | not recorded | See below |

`psnr_sycl` and `psnr_cuda` reduce only each plane's sum of squared errors on
the device. The host turns it into `psnr_*`, `mse_*` and `apsnr_*` with the
same helpers the CPU extractor uses (`core/src/feature/psnr_score.h`).

On Metal, a model that sets `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`
or `min_sse` computes `psnr` on the CPU instead
([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)). Naming the
twin with one of these options fails with `unknown option`.

From the CLI, `--backend sycl` (or `cuda`) runs `--feature psnr` with any of
these options on `psnr_sycl` (or `psnr_cuda`)
([ADR-1359](../adr/1359-cli-feature-backend-twin.md)); naming the twin,
`--feature psnr_sycl=...`, does the same:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend sycl --no_prediction --json --output out.json \
    --feature psnr=enable_mse=true:enable_apsnr=true:min_sse=0.5
```

`mse_y` / `mse_cb` / `mse_cr` then appear per frame and `apsnr_y` /
`apsnr_cb` / `apsnr_cr` under `aggregate_metrics`.

### How the `float_psnr` twins stay exact

The twins form the CPU's term (`diff * diff` in `float`) and add the terms as
integers, so their sums are exact, as the CPU's per-row sums are.

| Twin | Measured on | Frames identical at `--precision max` | Earlier error |
|---|---|---|---|
| `float_psnr_hip` | gfx1036 | 178 of 178, from 480x270 to 3840x2160 | Up to 7.6e-8 dB on high-bit-depth input with large differences (full-range noise) before 2026-10-01, when the twin added in single precision |
| `float_psnr_sycl` | Arc A380 | 288 of 288, from 576x324 to 3840x2160, at 8 to 16 bits | 269 identical before; up to 7.4e-8 dB off on full-range high-bit-depth content |
| `float_psnr_cuda` | RTX 4090 | see the table below | up to 1.2e-7 dB |

Measured for `float_psnr_cuda` on an RTX 4090 at `--precision max` against
`--backend cpu`, frames identical and the largest difference:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324 at 8 to 16 bits and 4:2:2, both 1080p checkerboards, Sparks 10 bit, BBB 3840x2160, noise at 8 bits | 167 of 167 | 167 of 167 |
| Full-range noise 576x324 at 10, 12 and 16 bits, 3 frames each | 0 of 9, 2.5e-8 dB | 9 of 9 |
| Bright 16-bit 1920x1080 (samples 56000 to 64000), 2 frames | 0 of 2, 7.6e-8 dB | 2 of 2 |
| BBB 1920x1080 widened to 16 bits, 40 frames | 1 of 40, 4.0e-8 dB | 40 of 40 |
| BBB 3840x2160 widened to 16 bits, 32 frames | 0 of 32, 4.0e-8 dB | 32 of 32 |
| Noise at 40x40, 56x56 and 64x64, 8 and 10 bits | 10 of 18, 1.2e-7 dB | 18 of 18 |

The same holds with `uncapped=true`.

#### Past 2^53 units

The CPU adds each row's squared differences, which is exact, and the rows
into one `double`, which rounds once the sum passes 2^53 units of
1 / scaler^2. A 16-bit frame whose mean squared error times its pixel count
passes 2^37 on the 8-bit scale reaches that point: 16570 at 3840x2160, a PSNR
below 6 dB.

Since [ADR-1499](../adr/1499-float-psnr-twins-cpu-row-order.md) the CUDA, SYCL
and HIP twins return the CPU's bits there too. Each block or work-group of the
kernel is a segment of one row, and the host adds each row's exact sum into a
`double` in the CPU's order (`core/src/feature/float_psnr_rows.h`). Measured at
`--precision max` against `--backend cpu` on an RTX 4090, an Arc A380 and a
gfx1036 alike:

| Fixture | Before | Now |
|---|---|---|
| 16-bit 3840x2160 noise, reference in the upper half of the range and distorted frame in the lower half, 8 frames | 0 of 8 (CUDA, 6.2e-15 dB; HIP, 3.3e-13 dB), 1 of 8 (SYCL) | 8 of 8 |
| 16-bit 3840x2160 noise, 16 frames; BBB 3840x2160 widened to 16 bits three ways, 32 frames each | identical | identical |

## Cost

The exact twins did not change the frame time. Per frame through the `vmaf`
tool, `float_psnr_cuda`, steady state (the time of 52, 40 or 32 frames less
the time of 4, per added frame), medians of 15 interleaved pairs of runs at a
load average of 7 to 10:

| Input | Before | After | Paired difference |
|---|---|---|---|
| 3840x2160, 8 bit | 1.91 ms | 1.90 ms | -0.15 ms |
| 1920x1080, 16 bit | 1.02 ms | 0.97 ms | +0.01 ms |
| 3840x2160, 16 bit | 4.64 ms | 4.37 ms | -0.24 ms |

For the past-2^53 change the frame time per 16-bit 3840x2160 frame, before and
after, pictures preloaded, medians of 5 interleaved runs:

| Device | Before | After |
|---|---|---|
| RTX 4090 | 4.12 ms | 4.12 ms |
| Arc A380 | 7.16 ms | 7.06 ms |
| gfx1036 | 10.9 ms | 9.3 ms |

## Notes and limitations

- The `psnr` extractor sets the temporal flag only because `apsnr`
  accumulates across the clip; the per-frame PSNR itself is stateless.
- `apsnr_*` has its own ceiling, `ceil(10 * log10(peak^2 * n_pixels))`,
  which is a true theoretical maximum rather than a truncation and is not
  affected by `uncapped`.
- A PSNR gap that looks like "28 dB where I expected 72 dB" is almost never
  this ceiling, because a `MIN` can only lower a value. Check frame alignment
  in the decode graph first.

## History

- **2026-10-03, ADR-1499.** The `float_psnr` CUDA, SYCL and HIP twins return
  the CPU's bits past 2^53 units. Before, they added every block of the frame
  into one exact total and rounded it once.
- **2026-10-01, ADR-1440.** `float_psnr_hip` adds the squared differences as
  integers. Before, it added in single precision.
- **ADR-1455 and ADR-1450.** `float_psnr_cuda` and `float_psnr_sycl` add
  integers too.
- **2026-09-30, ADR-1373 and ADR-1382.** `psnr_cuda` and `psnr_hip` implement
  the whole option table and see every frame under `--subsample`.
- **ADR-1193.** The `psnr_max` ceiling was historically also applied to every
  genuinely computed value, so any frame whose true PSNR exceeded it was
  silently reported as the ceiling. A 576x324 8-bit pair differing by a single
  luma step reported `psnr_y = 60.000000`. The `uncapped` option separates the
  two roles.

## See also

- [Feature overview](features.md): the full extractor table.
- [ADR-1193](../adr/1193-psnr-uncapped-option.md): why `uncapped` is opt-in.
- [PSNR-HVS](psnr-hvs.md): the perceptually weighted variant.
