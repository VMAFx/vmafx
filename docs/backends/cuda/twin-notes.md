<!-- markdownlint-disable MD013 MD060 -->

# CUDA twin notes

Per-twin reference for the CUDA backend: what each CUDA feature extractor
computes, which options it honours, how it agrees with the CPU extractor and
how to check it. Start with the [CUDA overview](overview.md) for the build,
the run-time flags and the summary table of agreement; this page holds the
detail behind each row. Dated before/after measurements are collected under
[History](#history) at the end.

| Twin | Section |
|------|---------|
| default model features (`cambi`, `speed_*`, `motion`, `adm`, `psnr`, ...) | [Default model feature dispatch](#default-model-feature-dispatch-vmaf_v1016_3d0h) |
| `adm` | [`integer_adm_cuda`](#integer_adm_cuda-runs-the-default-models-adm-on-the-device) |
| `vif`, `float_vif` | [`vif_cuda`](#vif_cuda-returns-the-cpus-scores-bit-for-bit) |
| `motion`, `motion_v2`, `float_motion`, `psnr`, `ssim`, tiny frames | [CPU parity](#cpu-parity-motion-options-and-tiny-frames) |
| `float_ssim` | [every scale](#float_ssim-runs-on-the-device-at-every-scale-adr-1399), [frame sums](#float_ssim-adds-its-frame-sums-in-the-cpus-order-adr-1464) |
| `float_ms_ssim` | [`float_ms_ssim` sums](#float_ms_ssim-adds-its-sums-in-the-cpus-order-adr-1465) |
| `float_moment` | [`float_moment_cuda`](#float_moment_cuda-matches-the-cpu-float_moment) |
| `psnr_hvs` | [`psnr_hvs_cuda`](#psnr_hvs_cuda-computes-chroma-by-default-adr-1203) |
| `cambi`, `speed_chroma`, `speed_temporal` | [device-resident pipelines](#cambi-and-speed-run-entirely-on-the-device-adr-1379-adr-1380), [SpEED-chroma](#speed-chroma-4k-launch-bounds-and-the-singular-vs-failure-contract-adr-1202) |
| `ssimulacra2` | [SSIMULACRA 2](#ssimulacra-2) |
| parity gate | [Exact twins declared as a group](#exact-twins-declared-as-a-group) |

## Default model feature dispatch (`vmaf_v1.0.16_3d0h`)

When running the default model `vmaf_v1.0.16_3d0h` under `--backend cuda`,
features are selectively dispatched between GPU and CPU based on option support
([ADR-1183](../../adr/1183-model-options-gate-gpu-twin-selection.md)):

### CAMBI and SpEED

`cambi_cuda` and `speed_chroma_cuda` run directly on the GPU. `cambi_cuda`
honours `cambi_high_res_speedup` (`hrs`), which the default
model sets to `1080`: at `>= 1080p` the twin resolves the option against the
encode pixel count, halves the adjusted window and runs one extra decimation
before scale 0, exactly as `core/src/feature/cambi.c` does. Measured parity,
per-frame `cambi_hrs_1080_cmxv_17_vlt_0.06` at `--precision max` (`%.17g`) on an
RTX 4090:

| Fixture | Frames | pooled cambi (CPU and CUDA) | max per-frame CPU↔CUDA delta |
| --- | --- | --- | --- |
| `src01` 576x324 | 48 | 0.2596781483085728 | 0 |
| Tennis 1920x1080 | 10 | 0.5670459080762581 | 0 |
| checkerboard 1px / 10px 1920x1080 | 3 each | 0 | 0 |

Every CAMBI GPU stage is integer-only and the c-value / pooling residual was the
CPU code called through `cambi_internal.h` (on the device since ADR-1379), so
the score agreed to every printed digit on these fixtures. That is a
measurement of those fixtures; the general contract is the exact-twin gate (see
the [overview](overview.md#numerical-agreement)). Pooled `vmaf` differed by
1.1e-5 on the Tennis pair when this was measured, because the ADM, VIF and
motion twins were not yet bit-identical (ADR-1416, ADR-1462, ADR-1372).

### Motion

`integer_motion_cuda` computes motion scores on device while
honoring `motion_max_val`, with the CPU's SAD arithmetic since
[ADR-1372](../../adr/1372-cuda-motion-diff-first-pipeline.md).

### ADM

`integer_adm_cuda` runs the default model's ADM on the device,
including `adm_csf_mode: 2`. `T-GPU-ADM-CSF-MODE-NOT-PORTED-2026-09-05` is
closed; see [the section
below](#integer_adm_cuda-runs-the-default-models-adm-on-the-device) for the full
option list.

### CIEDE2000

`ciede_cuda` has a CUDA kernel. It is not bit-identical to the
CPU because glibc's math library differs from CUDA's; the gate bounds it at 1e-9
([ADR-1426](../../adr/1426-cuda-ciede-cpu-arithmetic.md), `LIBM_TWINS`).

### PSNR

`psnr_cuda` ships with the full luma + chroma set (`psnr_y`,
`psnr_cb`, `psnr_cr`); luma landed in
[ADR-0182](../../adr/0182-gpu-long-tail-batch-1.md) batch 1b, chroma in
[ADR-0351](../../adr/0351-cuda-chroma-psnr.md) (T3-15(b)). YUV400P clamps to
luma-only at runtime. Cross-backend gate vs CPU is bit-exact
(`max_abs_diff = 0.0` at `places=4` on the 576×324 + 640×480 testdata fixtures,
RTX 4090, 8-bit 4:2:0).

### SSIM, MS-SSIM and PSNR-HVS

SSIM, MS-SSIM and PSNR-HVS have CUDA kernels
and participate in the cross-backend parity gate (`psnr_hvs_cuda` is compared
with the CPU exactly, tolerance 0, per
[ADR-1397](../../adr/1397-psnr-hvs-twins-cpu-float-sum.md)). The CUDA
`float_ansnr` extractor was removed together with its CPU twin in
[ADR-0709](../../adr/0709-vmafx-phase4b-distributed-platform.md) (PR #38); ANSNR
is no longer dispatched on any backend.

### Float twins (`float_*`)

The CUDA backend implements the float
twins for PSNR / Motion / VIF / ADM
([ADR-0202](../../adr/0202-float-adm-cuda-sycl.md)). Requesting
`--feature float_<x>` together with `--backend cuda` dispatches to GPU for those
metrics.

### `float_motion` options

The options `motion_add_scale1`, `motion_add_uv`,
`motion_filter_size` and `motion3_score` were added to the CPU `float_motion`
extractor by the upstream port from Netflix/vmaf
[`b949cebf`](https://github.com/Netflix/vmaf/commit/b949cebf) (2026-04-29). On
CUDA:

- `float_motion_cuda` takes `motion_max_val` since
  [ADR-1373](../../adr/1373-cuda-twin-cpu-option-parity.md).
- It emits `motion3_score` with the `motion_blend_factor` /
  `motion_blend_offset` options since `T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`.
- The other three options keep `float_motion` on the CPU.

### `integer_motion` options

As of T3-15(c) /
[ADR-0219](../../adr/0219-motion3-gpu-coverage.md), the kernel emits
`motion3_score` in 3-frame window mode via host-side `motion_blend()`
post-processing of `motion2_score`. The full options surface
(`motion_blend_factor`, `motion_blend_offset`, `motion_fps_weight`,
`motion_max_val`, `motion_moving_average`) is exposed.
`motion_five_frame_window=true` runs on the device too, on `motion_cuda` and
`motion_v2_cuda`, with the CPU's bits ([Motion, five-frame
window](../../metrics/motion.md#five-frame-window),
[ADR-1491](../../adr/1491-gpu-motion-five-frame-window.md)); one more raw luma
plane is kept while the option is set.

### `motion_add_uv` is not wired

`motion_add_uv=true` is not yet wired through to the CUDA backend. It is
independent from motion3. The CUDA `picture_copy()` callsite at
[`src/feature/cuda/integer_ms_ssim_cuda.c`](../../../core/src/feature/cuda/integer_ms_ssim_cuda.c)
passes `0` for the new trailing `channel` argument (Y plane only, preserving
CUDA pre-port behaviour). UV-plane motion on GPU is a follow-up tracked in
[docs/state.md](../../state.md).

### `psnr_hvs_cuda` reads the device pictures directly

The twin reads the device pictures directly (closes
`T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29`, the CUDA port of
[ADR-1369](../../adr/1369-sycl-shared-planes-light-twins.md)) — no host copy,
host conversion or float upload per frame. The kernel reads the raw 8- to 12-bit
samples with the picture pitch, runs two threads per 8x8 block (one per image,
exchanging their statistics with `__shfl_xor_sync`) with the DCT in shared
memory, and covers every plane in one launch.

### `psnr_hvs_cuda` returns the CPU's bits

`psnr_hvs_cuda` returns the CPU extractor's scores bit for bit
([ADR-1397](../../adr/1397-psnr-hvs-twins-cpu-float-sum.md), closes
`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`).

- The CPU adds every masked coefficient error of a plane into one running
  `float`, so its score depends on the order of the additions.
- The kernel stores the 64 terms of every block, computed in the CPU's
  arithmetic (the fatbin is built with `--fmad=false`), and the host adds them
  in the CPU's order.
- `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` equal
  `--backend cpu` at `--precision max` on 576x324, 1920x1080 and 3840x2160
  content at 8 to 12 bits. Before, the twin summed per block and was up to
  1.66e-2 dB away at 3840x2160.
- The price is a 256-byte readback per block and a sequential host sum: on an
  RTX 4090 a 3840x2160 frame takes 12.2 ms instead of 2.4 ms (1920x1080: 3.1
  instead of 0.6), more than sixteen CPU threads need (6.5 ms). Details, memory
  use and the open tuning row are on [the psnr_hvs
  page](../../metrics/psnr-hvs.md#gpu-twins).

See [feature metrics](../../metrics/features.md) for the per-extractor coverage
matrix.

## `integer_adm_cuda` runs the default model's ADM on the device

The default model `vmaf_v1.0.16_3d0h` requests
`VMAF_integer_feature_adm3_score` with
`adm_csf_mode=2` (Barten/Watson blend), `adm_dlm_weight=0.7`,
`adm_enhn_gain_limit=1.0`, `adm_min_val=0.5` and `adm_noise_weight=0.02`,
and looks the result up under the key
`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.

`integer_adm_cuda` honours every one of those options, so the whole ADM
family runs on the device for the default model:

- `adm_csf_mode` selects the CSF weights that feed `rfactor` / `i_rfactor`
  (Watson97 `0`, Barten `1`, Barten/Watson blend `2`, Barten/Watson blend MAE
  `3`), exactly as `integer_adm.c::adm_csf_factors()` does. The
  `{36453, 36453, 49417}` scale-0 fixed-point constants are used only under
  `adm_csf_mode=0` at the canonical `nvd × rdh`, matching the CPU.
- `adm_p_norm` is the exponent of the contrast-measure pooling in
  `conclude_adm_cm()`.
- `adm_dlm_weight` / `adm_skip_aim` drive the ADR-0746 AIM device pass and the
  adm3 blend.
- The `VmafOption` table is an entry-for-entry mirror of the CPU table, so the
  emitted feature-name key is identical to the CPU twin's.

Two CPU-parity corrections landed with it: `adm_min_val` no longer clamps
`adm2` (the CPU floors the adm3 expression only), and the `numden_limit`
precision floor scales with the full-frame area rather than the scale-3 area.

Run it with:

```bash
./core/build/tools/vmaf --backend cuda \
  --model version=vmaf_v1.0.16_3d0h --no_prediction \
  --reference ref.yuv --distorted dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 --json
```

## `vif_cuda` returns the CPU's scores bit for bit

The fixed-point `vif` is integer arithmetic except for its logarithms, which
the CPU extractor reads from a table of 32768 values built with the host math
library. `vif_cuda` reads the same table
([ADR-1462](../../adr/1462-cuda-vif-reads-host-log2-table.md)): the host
builds it once when the extractor starts and copies it to the device, and the
kernels look every logarithm up. They compute none themselves, so the twin
returns the CPU's scores whatever math library the host has and whatever the
CUDA release's device `log2f()` returns.

Until 2026-10-02 the kernels evaluated `log2f()` on the device. On an RTX
4090 with CUDA 13.4 and glibc 2.44 that gave the CPU's table on all 32768
values, although the device's `log2f()` differs from glibc's by one unit in
the last place for 307 arguments; on an AMD GPU the same construction had
moved 77 values (ADR-1435). No score changes on this host with the switch.

### Measured agreement

Measured on an RTX 4090 at `--precision max` against `--backend cpu`:

| Fixture | Scores identical |
|---|---|
| Netflix 576x324 at 8 and 10 bits, both 1080p checkerboards, BBB 3840x2160 (200 frames) | 1028 of 1028 |
| Netflix 576x324 at 12 and 16 bits and as 10-bit 4:2:2, Sparks at 10 bits, noise at 8 to 16 bits, a bright 16-bit 1080p pair | 292 of 292 |
| Noise at 40x40, 56x56 and 64x64, 8 and 10 bits | 72 of 72 |
| `debug=true`, `vif_enhn_gain_limit=1.0`, `vif_skip_scale0` on 196 frames | 4508 of 4508 |

The frame time did not change measurably. Per frame through the `vmaf` tool,
steady state (the time of 60 or 84 frames less the time of 4, per added
frame), medians of 25 interleaved pairs of runs; the host's load average was
70 to 90 from other builds, so the samples are wide:

| Input | Before | After | Paired difference |
|---|---|---|---|
| 1920x1080, 8 bit | 0.68 ms | 0.61 ms | -0.09 ms |
| 3840x2160, 8 bit | 2.30 ms | 2.30 ms | -0.09 ms |

```shell
build-cuda/test/test_cuda_vif_log2_table
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-cuda/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu cuda --features vif
```

## CPU parity: motion, options and tiny frames

Three changes bring CUDA twins to their CPU extractors' arithmetic and options.
An RTX 4090 run confirmed them; each row that `docs/state.md`
names below carries the commands and the measured results:

| Check on an RTX 4090 | Result |
|---|---|
| `motion` against the CPU, Netflix pair and 50 frames of 3840x2160 | 0.0 (a `master` build: 1.26e-5 and 6.9e-5) |
| `psnr` with `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse`, also with `--subsample 2` | identical, `apsnr_*` included |
| `motion_v2` and `float_motion` with `motion_fps_weight` and `motion_max_val` | identical |
| `ssim` with `enable_db` / `clip_db` | within 7.3e-13 dB; identical since [ADR-1424](../../adr/1424-cuda-ssim-cpu-frame-sum.md) |
| `float_ssim` with `enable_lcs` / `enable_db` | within 6.9e-6 dB (1.8e-7 linear); identical since [ADR-1399](../../adr/1399-cuda-float-ssim-device-decimation.md) |
| `float_ssim` with `enable_db`, identical flat 64x64 frames | 72.247198959355487 dB on both |
| `compute-sanitizer` on the ADM and VIF tiny-frame tests | 0 errors |

### `motion_cuda` blurs the frame difference, like the CPU

The CPU `motion` extractor sums `|blur(prev - cur)|`, rounding after the
vertical and after the horizontal pass. `motion_cuda` blurred each frame and
summed `|blur(cur) - blur(prev)|`, which is the same sum only without rounding.
It now runs the kernel `motion_v2_cuda` already used, through one host helper
(`integer_motion_sad_cuda.c`), so its SAD is the CPU's
([ADR-1372](../../adr/1372-cuda-motion-diff-first-pipeline.md),
`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`). With it:

- the debug `integer_motion` score is the CPU's SAD score, weighted by
  `motion_fps_weight` and capped at `motion_max_val`; before, it was the raw
  normalised SAD;
- each frame's copy of the reference luma waits for the previous frame on the
  device, through an event, never on the host, and the eight-frame batch
  readback ([ADR-0845](../../adr/0845-cuda-motion-launch-overhead.md)) waits
  once instead of twice;
- `motion_v2_cuda`'s SAD is unchanged (same kernel); its host scoring now
  weights and caps it like the CPU (see the option section below);
- with `motion_force_zero`, `motion_cuda` and `float_motion_cuda` publish zeros
  from the first frame. Their `init()` switches them to a synchronous
  `extract()`, and the engine used to call the cleared `submit()` on the first
  frame and crash (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`); it
  now initialises an extractor before it picks the path.

Check it on the Netflix pair (prints `0.0`; a build from before the change
prints about 1.3e-5):

```bash
Y=python/test/resource/yuv
for b in cpu cuda; do
  vmaf -r $Y/src01_hrc00_576x324.yuv -d $Y/src01_hrc01_576x324.yuv \
      -w 576 -h 324 -p 420 -b 8 --no_prediction --feature motion \
      --backend $b --precision=max --json -q -o /tmp/motion_$b.json
done
python3 -c "import json; a, b = (json.load(open(f'/tmp/motion_{x}.json'))['frames'] for x in ('cpu', 'cuda')); print(max(abs(p['metrics']['integer_motion2'] - q['metrics']['integer_motion2']) for p, q in zip(a, b)))"
```

### `motion_cuda` emits the CPU's SAD score

The CPU `motion` extractor writes `VMAF_integer_feature_motion_sad_score` on
every frame: the frame's SAD, weighted by `motion_fps_weight` and capped at
`motion_max_val`, 0 on the first frame and with `motion_force_zero`.
`motion_cuda` computed that value and published it only as the debug
`integer_motion` score, so the result of `--backend cuda --feature motion`
lacked a key the CPU result has. It now writes the SAD score on every frame.

On an RTX 4090 at `--precision max` it equals the CPU's on 348 of 348 frames
(576x324 to 3840x2160, 8 to 16 bits, 40x40 to 64x64 noise), also with
`debug`, `motion_force_zero`, `motion_moving_average` and weight, blend and
cap options. `motion2` and `motion3` are unchanged, and so is the frame time:
the kernel is the same.

### CPU options on the PSNR, SSIM and float-motion twins

Four CUDA twins take their CPU extractor's full option table
([ADR-1373](../../adr/1373-cuda-twin-cpu-option-parity.md)). Before, a model
that set one of these options computed the feature on the CPU
([ADR-1183](../../adr/1183-model-options-gate-gpu-twin-selection.md)), and
naming the twin with the option failed with `unknown option`.

| Twin | Options added | Where the option acts |
|---|---|---|
| `psnr_cuda` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | host, on the device-reduced SSE, through `psnr_score.h` (bit-exact with the CPU) |
| `integer_ssim_cuda` | `enable_db`, `clip_db` | host, on the device-reduced score (`vmaf_ssim_max_db()`) |
| `float_ssim_cuda` | `enable_lcs`, `enable_db`, `clip_db` | `enable_lcs`: a second pass-2 kernel also stores each window's L, C and S, which the host adds; dB on the host |
| `float_motion_cuda` | `motion_max_val` (`mmxv`), `motion_blend_factor` (`mbf`), `motion_blend_offset` (`mbo`) | host: every emitted score, the debug `motion` included, is weighted by `motion_fps_weight` and then capped; `motion3` is the CPU's blend of `motion2` (`motion_blend_clip()`) |

#### Scoring fixes

The same change fixed how these twins score, not only which options they take:

- **`float_ssim_cuda` computes the CPU's `l * c * s`.** Each pixel uses the
  CPU's types and rounding points (double numerators over fp32 denominators,
  `s` in fp32), and the frame mean is rounded to fp32 as the CPU returns it.
  With `enable_db`, identical frames therefore report what the CPU reports:
  `+inf` where its mean rounds to 1, a finite value where it does not
  (identical flat frames: 72.247199 dB, not `+inf`). The default score moves
  towards the CPU's by an fp32 rounding.
- **`float_ssim_cuda` still accepts `enable_chroma`**, which the CPU
  `float_ssim` does not have: it has always been ignored (the twin scores luma
  only), and now logs a warning that says so.
- **`integer_ssim_cuda` computes each pixel's term as the CPU does**, without
  FMA contraction and with the CPU's grouping, so every term equals the CPU's
  bit for bit. Since [ADR-1424](../../adr/1424-cuda-ssim-cpu-frame-sum.md) the
  device stores the terms and the host adds the plane in the CPU's raster
  order, so the score is bit-identical to the CPU's (`ssim` is an exact
  twin).
- **`motion_v2_cuda` publishes the CPU's SAD score**: fps-weighted and capped
  at `motion_max_val`, with `motion2_v2` and `motion3_v2` derived from it and
  `0` / `0` for a one-frame input. Before, it stored the raw SAD and did not
  cap `motion2_v2`, so non-default `motion_fps_weight` / `motion_max_val`
  gave other scores than the CPU.
- **`psnr_cuda` sees every frame under `--subsample`**, like the CPU `psnr`,
  so `enable_apsnr` sums all frames.

```bash

# psnr_cuda with the CPU options (mse_* per frame, apsnr_* under aggregate_metrics)
vmaf --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend cuda --no_prediction --json --output out.json \
    --feature psnr=enable_mse=true:enable_apsnr=true:min_sse=0.5
```

### Tiny frames: integer ADM rows and the VIF minimum

- **Integer ADM** keeps the CPU's 17x17 minimum. The rows and taps its DWT
  kernels load now come from `integer_adm/adm_dwt2_rows.h`, which the
  device-free `test_cuda_adm_dwt2_rows` replays for every plane height up to
  8192: from 17 rows up every load is inside the plane, and the scale-0 load
  is clamped into the plane for the padding threads below that
  ([ADR-1374](../../adr/1374-cuda-integer-tiny-frame-guards.md),
  `T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29`). Scores do not move.
- **Integer VIF** (`vif_cuda`) needs 16 pixels in each dimension, like
  `vif_sycl`: every scale reflects its taps once. Under model dispatch and
  `--backend cuda --feature vif`, smaller frames are computed by the CPU `vif`
  and match it bit for bit; `--feature vif_cuda` on such a frame fails with
  `vif_cuda requires width >= 16 and height >= 16`
  (`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`).

## `float_ssim` runs on the device at every scale (ADR-1399)

CPU `float_ssim` reduces both pictures before it scores them: by
$\max(1, \operatorname{round}(\min(w, h) / 256))$, which is 1 below a 384-pixel short side, 4
at 1920x1080 and 8 at 3840x2160, or by the `scale` option. `float_ssim_cuda`
used to compute scale 1 only, so at 1080p and 4K `--backend cuda --feature
float_ssim` and models ran the CPU extractor and printed a fallback warning.
It now reduces on the device, with the CPU's arithmetic
([ADR-1399](../../adr/1399-cuda-float-ssim-device-decimation.md)):

```shell
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend cuda --feature float_ssim --json -o out.json

# feature_backends lists float_ssim_cuda; no warning
vmaf ... --backend cuda --feature float_ssim=scale=3:enable_lcs=true
```

### What you can rely on

What you can rely on:

- **Any `scale` from 1 to 10 and the automatic one.** The twin falls back to
  the CPU (or fails, when you name `float_ssim_cuda` yourself) only when the
  reduced picture is smaller than SSIM's 11x11 window, for example 100x100
  at `scale=10`.
- **The CPU's score.** The reduced pictures are the CPU's byte for byte, and
  both Gaussian passes add in double precision as the CPU does. The frame sum
  is the CPU's as well since
  [ADR-1464](../../adr/1464-cuda-float-ssim-raster-order-sum.md); the next
  section describes it. On an RTX 4090 every frame equals `--backend cpu` at
  `--precision max`:
  - the Netflix 576x324 pair at 8, 10, 12 and 16 bits and scales 1 to 10;
  - the 1920x1080 checkerboard pairs, a 1920x1080 pair and BBB 3840x2160 at
      8 and 10 bits;
  - with `enable_lcs`, `enable_db` and `clip_db` too.

  Before, the twin was 1 to 3 units in the last fp32 place from the CPU on
  every frame.
- **No host pass over the picture.** The kernels read the uploaded picture;
  each frame is three kernels (two at scale 1) and one result read-back, of
  one `double` per scored window.

### Cost

Cost on an RTX 4090, BBB 3840x2160 8-bit (load average 16 to 19):

| Request | Before | After |
|---|---|---|
| `--backend cuda --feature float_ssim` (automatic scale 8), per frame through the CLI | 18.9 ms (CPU fallback) | 3.0 ms |
| the same on `--backend cpu --threads 16` | 11.0 ms | 11.0 ms |
| GPU time of the kernels at the automatic scale | — | 88 us |
| `--feature float_ssim_cuda=scale=1`, per frame through the CLI | 3.1 ms | 3.9 ms |
| GPU time of the kernels at `scale=1` | 0.71 ms | 3.58 ms |

An explicit `scale=1` on a large picture is the one case that got slower:
the double-precision sums then run over the full picture. The CPU extractor
takes 43.8 ms per frame for the same request on 16 threads.

## `float_ssim` adds its frame sums in the CPU's order (ADR-1464)

CPU `float_ssim` adds the SSIM value of every window into one
double-precision sum, left to right and top to bottom, and reports the mean
as a `float`. `float_ssim_cuda` computed the same values and added them in
blocks. A sum of floating-point numbers depends on its order in its last
bits, and on rare frames that is enough to round the mean to the next
`float`: a search over noise frames found two in 31 million. On one of them
the CPU reports -4.222829943500983e-07 and the twin reported
-4.222829659283889e-07.

The twin now adds in the CPU's order
([ADR-1464](../../adr/1464-cuda-float-ssim-raster-order-sum.md)): the device
stores every window's value, the host reads them back and adds them one after
the other. With `enable_lcs` the luminance, contrast and structure sums are
formed the same way.

### What changes for you

What changes for you:

- **Scores.** `float_ssim`, and `float_ssim_l`, `_c` and `_s`, equal
  `--backend cpu` on every input, the frame above included. On content you
  have measured before nothing moves, unless one of your frames is such a
  rare case; then it moves by one `float` step to the CPU's value.
- **Time, where the picture is scored at full size.** That is the automatic
  scale for pictures with a short side below 384 pixels, and an explicit
  `scale=1` on anything larger. The cost is about 1 ns per window (3.3 ns
  with `enable_lcs`). At the automatic scale of 1080p and 4K input the scored
  picture is 480x270 and nothing measurable changes.

  | Input and request | Before | After |
  |---|---:|---:|
  | 576x324 | 0.14 ms | 0.33 ms |
  | 576x324, `enable_lcs` | 0.15 ms | 0.75 ms |
  | 1920x1080 | 1.12 ms | 1.22 ms |
  | 3840x2160 | 3.53 ms | 3.69 ms |
  | 1920x1080, `scale=1` | 0.88 ms | 3.10 ms |
  | 3840x2160, `scale=1` | 3.91 ms | 12.55 ms |
  | 3840x2160, `scale=1`, `enable_lcs` | 4.19 ms | 30.92 ms |

  Per frame through the `vmaf` tool on an RTX 4090, medians of 11 to 15
  alternating pairs at a host load average of 60 to 80; the 1080p and 4K rows
  at the automatic scale differ by less than their spread.
- **Memory at `scale=1`.** One `double` per window on the device and as
  pinned host memory, four with `enable_lcs`: 66 MB (263 MB) for a 3840x2160
  frame, 1 MB (4 MB) at the automatic scale.

### Check it

Check a build with the frame the fix was written for:

```shell
build/test/test_cuda_float_ssim_order
```

Reproduce the parity and the timing:

```shell
python3 scripts/dev/speed_gpu_parity.py --backend cuda --feature float_ssim \
    --vmaf "$PWD/build/tools/vmaf"
```

## `float_ms_ssim` adds its sums in the CPU's order (ADR-1465)

CPU `float_ms_ssim` scores five scales. On each it adds the luminance,
contrast and structure value of every window into three double-precision
sums, left to right and top to bottom, and takes each mean as a `float`.
`float_ms_ssim_cuda` used to add the same values in blocks; a sum of
floating-point numbers depends on its order in its last bits, and on rare
frames that rounds a mean to the next `float`. The measured cases are in
[History](#history).

The twin now adds in the CPU's order
([ADR-1465](../../adr/1465-cuda-float-ms-ssim-raster-order-sum.md)): the
device stores the three values of every window of every scale, the host
reads them back and adds them one after the other.

### What changes for you

What changes for you:

- **Scores.** `float_ms_ssim` and the fifteen `enable_lcs` outputs equal
  `--backend cpu` on every measured input, those frames included. On content
  you have measured before nothing moves unless one of your frames is such a
  rare case; then the last digits move to the CPU's.
- **Time.** About 2.1 ns per scored window, on every frame:

  | Input | Before | After |
  |---|---:|---:|
  | 576x324 | 0.33 ms | 0.81 ms |
  | 1920x1080 | 2.70 ms | 8.80 ms |
  | 3840x2160 | 10.92 ms | 33.71 ms |

  Per frame through the `vmaf` tool on an RTX 4090, medians of 11
  alternating pairs. `enable_lcs` costs nothing extra.
- **Memory.** 20 bytes per window on the device and as pinned host memory:
  54 MB at 1920x1080, 219 MB at 3840x2160.

Check a build with the frame the fix was written for:

```shell
build/test/test_cuda_float_ms_ssim_order
```

## `float_moment_cuda` matches the CPU `float_moment`

`float_moment_cuda` returns the CPU extractor's four moments bit for bit on
every frame ([ADR-1453](../../adr/1453-cuda-float-moment-cpu-float-squares.md)
for 16-bit squares, after ADR-1447 for the HIP twin and ADR-1449 for the SYCL
twin;
[ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md) for sums
past $2^{53}$ units). The parity gate compares the twin with tolerance 0.

### How it matches

How it matches:

- **Squares.** The CPU forms each sample's square in `float` before adding
  it. Up to 12 bits that is the integer square; at 16 bits it is the square
  rounded to 24 bits. The 16-bit kernel adds the `float` square, an integer
  below $2^{32}$, into `uint64` sums.
- **Sums.** The CPU adds the float squares into one `double` in raster
  order.
  - Below $2^{53}$ units of $2^{-16}$ that sum is exact and equal to the twin's
      integer sum. This covers every frame of up to 2 097 152 pixels and
      every 8-, 10- and 12-bit frame; those frames run no extra work.
  - On a larger 16-bit frame whose sum passes $2^{53}$ the CPU rounds as it
      adds, so the twin forms the CPU's rounded sum: four more kernels add
      each row exactly while the sum is at or below $2^{53}$, then from integer
      increments of the sum's last place, composed in pixel order and checked
      against the exact running sum, and term by term where a row crosses
      into the next binade (`core/src/feature/float_moment_sum.h`).

### Check it

Check it:

```shell
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference ref_16bit_3840x2160.yuv --distorted dis_16bit_3840x2160.yuv \
    --width 3840 --height 2160 --bitdepth 16 --backends cpu cuda --features float_moment
```

### Measurements

Measured on an RTX 4090 at `--precision max` against `--backend cpu`, frames
whose second moments are identical and the largest difference. The
repository's 16-bit Netflix clip is 8-bit content shifted left, whose squares
have few significant bits, so the usual fixtures never showed the 16-bit
defect.

| Fixture | Before the 16-bit change | Before the $2^{53}$ change | Now |
|---|---|---|---|
| Netflix 576x324 at 8 to 16 bits and 4:2:2, both 1080p checkerboards, Sparks 10 bit, BBB 3840x2160, noise at 8, 10 and 12 bits | 173 of 173 | 173 of 173 | 173 of 173 |
| Full-range noise 576x324, 16 bit, 3 frames | 0 of 3, 2.8e-5 | 0 of 3 | 3 of 3 |
| Bright 16-bit 1920x1080 (samples 56000 to 64000), 2 frames | 0 of 2, 1.0e-4 | 0 of 2 | 2 of 2 |
| BBB 1920x1080 widened to 16 bits, 40 frames | 0 of 40, 7.5e-5 | 0 of 40 | 40 of 40 |
| BBB 3840x2160 widened to 16 bits, 32 frames, 17 of them past $2^{53}$ | 0 of 32, 3.9e-5 | 32 of 32 | 32 of 32 |
| Full-range 16-bit noise 3840x2160, nine tenths near the peak, 16 frames | not measured | 0 of 16, 2.7e-7 | 16 of 16 |
| The same at 7680x4320, 4 frames | not measured | 0 of 4, 5.1e-7 | 4 of 4 |

BBB widened to 16 bits (shifted left by 8, times 257, full range with a
dithered low byte) was identical before the $2^{53}$ change as well: its samples
have no bits below the sum's last place until $2^{55}$, which a 3840x2160 frame
cannot reach. The parity gate's `float_moment` cell reads 0 at tolerance 0 on
the 16-bit 3840x2160 noise (it failed there before).

### Frame time

The frame time is unchanged. Per frame through the `vmaf` tool, steady state
(the time of 40 or 32 frames less the time of 4, per added frame), medians of
15 interleaved pairs of runs with a load average of 24 to 34 from other
builds on the machine:

| Input | Before | After | Paired difference |
|---|---|---|---|
| 1920x1080, 16 bit | 1.18 ms | 1.07 ms | -0.10 ms |
| 3840x2160, 16 bit | 4.77 ms | 5.25 ms | +0.04 ms |
| 3840x2160, 8 bit (kernel unchanged) | 2.01 ms | 1.96 ms | -0.03 ms |

After the $2^{53}$ change, time per 16-bit 3840x2160 frame through libvmaf,
pictures preloaded, medians of 5 interleaved runs at a load average of 4 to
5, before and after:

| Input | Before | After |
|---|---|---|
| Noise, every frame past $2^{53}$ | 5.28 ms | 5.31 ms |
| BBB full range, 17 of 32 frames past $2^{53}$ | 5.31 ms | 5.29 ms |

## `psnr_hvs_cuda` computes chroma by default (ADR-1203)

`psnr_hvs` is the YCbCr-weighted score `0.8*Y + 0.1*(Cb + Cr)`. The fork's
`enable_chroma` option is an opt-*out* for callers who want the cheaper
luma-only value, and every backend defaults it to on — the CPU and SYCL twins
default it to `true`, and the HIP twin computes chroma unconditionally.

Two consequences for callers:

- `psnr_hvs_cuda` scores change. If you were relying on the old default you
  were getting luma-only; pass `psnr_hvs_cuda=enable_chroma=false` to keep it.
- The extractor now dispatches three planes instead of one, so it costs more
  GPU time than before.

## CAMBI and SpEED run entirely on the device (ADR-1379, ADR-1380)

`cambi_cuda`, `speed_chroma_cuda` and `speed_temporal_cuda` no longer hand
work back to the host inside a frame
([ADR-1379](../../adr/1379-cuda-cambi-device-resident-pipeline.md),
[ADR-1380](../../adr/1380-cuda-speed-device-resident-pipeline.md), ports of the
SYCL designs of ADR-1357 and ADR-1358). Before, `cambi_cuda` downloaded the
distorted picture, preprocessed it on the host and read the image and mask back
at each of five scales for the host c-values and top-K pooling; the SpEED twins
copied their planes to the host, filtered them there, and read the covariance,
the independent terms and the per-block entropies back around a host eigenvalue
problem and QR solve.

### Per-frame cost

Per frame now, counted on an RTX 4090 with a CUPTI driver-API callback over
frames 13 to 22 of the Netflix 576x324 pair:

| Twin | Kernel launches | Read back | Other work | Host waits |
|---|---|---|---|---|
| `cambi_cuda` | 65 (66 with the input validation that 9- to 15-bit input gets) | 88 bytes: five top-K sums and a status word | two device memsets | one, in `collect()` |
| `speed_chroma_cuda` | 7 (8 with prescale) | 40 bytes: two scores and the singular / iteration-cap flags | four device-to-device plane copies | one, in `collect()` |
| `speed_temporal_cuda` | 7 (8 with prescale) | 40 bytes | two device-to-device plane copies (the previous frame stays on the device) | one, in `collect()` |

None of them uploads anything per frame: they read the planes the engine
already uploaded. Because nothing waits in `submit()`, their frames overlap with
the other CUDA extractors like any submit/collect twin.

### Scores

Scores:

- `cambi_cuda` equals `--backend cpu` to the last bit whenever the CPU's own
  `double` top-K sum is exact; otherwise the device holds the exact value and
  the CPU its rounding (at most 3.0e-13 on a heavily banded 4K clip). It now
  also rejects an adjusted window above 65 x 65, as `cambi.c` does.
- `speed_chroma_cuda` and `speed_temporal_cuda` equal the CPU extractor of
  the same build to the last bit, on a GCC build and on an icx build. The
  device runs `speed.c` up to the per-block variances; the host forms the
  entropies and the score from one block read back per frame, with `speed.c`'s
  own `log2()` calls ([ADR-1477](../../adr/1477-speed-upstream-double-math.md)).
  That holds for every `speed_prescale_method`: the `lanczos4` weights are
  read from a table the host builds with the CPU scaler's own routine. The
  CPU build must not fuse multiply-adds (no `-march=native` with icx).
  Details: [SpEED](../../metrics/speed_qa.md#where-a-twin-computes-what).

### Measurements

Measured on an RTX 4090 against an icx build of the CPU extractors, every
per-frame `cambi`, `speed_chroma_u/v/uv` and `speed_temporal` value at
`--precision max` is identical to `--backend cpu` on the Netflix 576x324 pair
and on BBB 3840x2160.

Before, `cambi_cuda` needed 11 device-to-host copies and 7 waits per frame,
`speed_chroma_cuda` 20 and 6, and `speed_temporal_cuda` failed at 1920x1080
and above. Milliseconds per frame at 3840x2160, before -> after:

| Twin | Before | After |
|---|---:|---:|
| `cambi_cuda` | 64.71 | 6.01 |
| `speed_chroma_cuda` | 24.90 | 6.89 |
| `speed_temporal_cuda` | fails | 5.88 |

Method, the 576x324 numbers, the sanitizer runs and the before/after transfer
counts are in
[Research-1379](../../research/1379-cuda-cambi-speed-device-resident.md).

## SpEED-chroma: 4K launch bounds and the singular-vs-failure contract (ADR-1202)

The backward-substitution solve launches one warp per linear system, and a
4K chroma plane exceeds 256 systems. The launch geometry below keeps the
block fixed and scales the block count with the picture.

The backward-substitution kernel launch maps one warp to one linear system
and packs `SC_SOLVE_WARPS_PER_BLOCK` (8) warps into a block. The block size is
therefore fixed at 256 threads and the *block count* is what scales with the
picture. Getting that the other way round puts the launch past CUDA's
1024-thread block limit on any large picture; it fails with
`CUDA_ERROR_INVALID_VALUE` and the output buffer keeps whatever was in it.

### The failure contract

**A device failure now fails the frame.** All three GPU twins previously
treated any non-zero return from their linear-algebra helper as "singular
covariance matrix" and imputed the `uv` score from the other chroma channel.
That rule is borrowed from the CPU extractor, where a non-zero return really
does mean singular.

In the GPU twins it never did: they handle singularity internally (warn, zero
the solution, return 0) and use the return value for API errors only. So a
real device error was fed into the imputation, and with both chroma channels
failing it averaged to `0.0` and reported success.

Singularity now travels in its own `bool *singular_out` and hard errors
propagate. Two consequences for callers:

- A CUDA, SYCL or HIP failure inside SpEED-chroma surfaces as a non-zero exit
  instead of a `0.0` score. If you have hardware that was already failing,
  you will now see the error rather than a plausible-looking number.
- The singular-matrix imputation documented for the CPU extractor is now
  actually in effect on the GPU backends, along with the CPU rule that a
  channel with exactly one singular side (reference or distorted) scores 0
  rather than an inflated value.

Measured on a 3840x2160 pair after the fix: CPU 67.150063, CUDA 67.150063,
SYCL 67.150065. The Netflix 576x324 pair is unchanged.

Note that the GPU parity tests in the repository runner's `--suite=fast`
selection all run below the
256-system threshold, so they cannot catch this class of defect. Check 4K
agreement against the CPU backend by hand when changing these kernels.

## SSIMULACRA 2

`ssimulacra2_cuda` shipped per
[ADR-0206](../../adr/0206-ssimulacra2-cuda-sycl.md) and has run the whole
frame on the device since
[ADR-1391](../../adr/1391-cuda-ssimulacra2-device-resident.md).

What the twin does per frame:

1. Reads the planes of the device pictures directly (the twin makes no copy
   of its own).
2. Converts YUV to linear RGB and XYB.
3. Runs the five blurs of each scale in one horizontal and one vertical
   launch.
4. Sums the per-pixel SSIM and edge-difference terms in fp64 in the CPU's
   order ([ADR-1433](../../adr/1433-cuda-ssimulacra2-cpu-sum-order.md)).
5. Downsamples, and copies one 864-byte block of per-scale sums to the host,
   where `collect()` waits once and pools the score.

The device kernels build with `--fmad=false`, like every CUDA kernel
([ADR-1403](../../adr/1403-cuda-strict-fp-every-kernel.md)), so everything up
to the sums matches the CPU bit for bit. Since ADR-1433 the sums do too: the
CPU adds each term pixel after pixel into one double, and the device returns
the bits of that loop from whole-number increments it forms per 1024-pixel
chunk, adding term by term only the chunks in which the running sum passes a
power of two. The per-frame score equals `--backend cpu` on every measured
frame (7.3e-11 at most before).

On an RTX 4090 a 3840x2160 frame takes 15.6 ms (7.8 ms with the tree sum,
about 720 ms before ADR-1391). 4:0:0 input and frames below 8x8 go to the CPU
extractor. Check and time it (the script wants an absolute `--vmaf` path):

```shell
python3 scripts/dev/speed_gpu_parity.py --backend cuda --feature ssimulacra2 \
    --vmaf "$PWD/build/tools/vmaf" \
    --netflix-dir python/test/resource/yuv --bbb-dir testdata/bbb
```

## Exact twins declared as a group

Six more CUDA twins are held to the CPU extractor's bits by the parity gate
([ADR-1457](../../adr/1457-cuda-exact-twins-declared.md)): `motion_cuda`
(also with `debug=true`), `motion_v2_cuda`, `psnr_cuda`, `float_ssim_cuda`
and `float_ms_ssim_cuda` (with and without `enable_lcs`) and `cambi_cuda`.
Each reaches the CPU's value by construction, and a sweep on an RTX 4090
measured every output identical at `--precision max`:

| Fixtures | Frames | Result |
|---|---|---|
| Netflix 576x324 at 8 and 10 bits, both 1080p checkerboards, BBB 3840x2160 | 105 | identical |
| Netflix 576x324 at 12 and 16 bits and as 10-bit 4:2:2, Sparks at 10 bits, full-range noise at 8 to 16 bits, a bright 16-bit 1080p pair | 73 | identical |
| Noise at 40x40, 56x56 and 64x64, 8 and 10 bits | 18 | identical (`float_ms_ssim` and `cambi` refuse these sizes, as the CPU does) |
| BBB 3840x2160, 200 frames | 200 | identical |
| 18 option sets on five fixtures | 59 each | identical |

The sweep covered all 21 gate features; the full table, with the three
defects it found and their fixes, is in
[Research-1457](../../research/1457-cuda-twin-exactness-sweep.md). With the
twins made exact by their own changes, the gate compares every CUDA feature
with tolerance 0 except `ciede` (within 5.2e-12 on the measured frames,
bound 1e-9), which differs from the CPU by the math library only. `speed_chroma`
is exact too since ADR-1477.

### Two things to know

Two things to know:

- `float_ssim` and `float_ms_ssim` are exact on every input: since
  ADR-1464 and ADR-1465 the host adds the CPU's per-window terms in the CPU's
  order (see the two sections above).
- A twin on this list that drifts is fixed. It does not get a tolerance.

```shell
build-cuda/test/test_cuda_exact_twins
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-cuda/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu cuda \
    --features motion motion_debug motion_v2 psnr float_ssim float_ssim_lcs \
               float_ms_ssim float_ms_ssim_lcs cambi
```

## Agreement notes: float_ms_ssim, float_motion, motion and SpEED

### `float_ms_ssim_cuda`

`float_ms_ssim_cuda` became bit-identical with ADR-1403. Besides the build
flag, its kernels follow `ms_ssim_decimate.c`, `iqa_convolve()` and
`ssim_accumulate_default_scalar()` operation for operation, and the host
rounds each per-scale mean to fp32 as the CPU does. Its `enable_lcs` outputs,
which were up to 1.3e-6 from the CPU, are identical too. Since
[ADR-1465](../../adr/1465-cuda-float-ms-ssim-raster-order-sum.md) the host
also adds the per-window terms of every scale in the CPU's order, which makes
that hold on every input; see
"`float_ms_ssim` adds its sums in the CPU's order" above.

### `float_motion_cuda`

`float_motion_cuda` is bit-identical since
[ADR-1409](../../adr/1409-float-motion-twins-cpu-float-sum.md). The CPU
extractor adds the absolute differences of a row into one `float`, the row
sums into another, and divides in `float`; the result depends on that order.
The twin adds each row on the device in the CPU's order, one thread per row,
and the host adds the rows (`core/src/feature/float_motion_sad.h`).

- `motion`, `motion2` and `motion3` match on every frame, also at 10 and 12
  bits and with the `motion_fps_weight`, `motion_max_val` and blend options
  set.
- The time per 3840x2160 frame did not change measurably (3.00 and 2.98 ms).
- The parity gate compares this twin with tolerance 0
  ([cross-backend gate](../../development/cross-backend-gate.md)).
- Before, the twin summed each 16x16 block on the device and the blocks in
  `double` on the host, which left it 3.1e-6 from the CPU on the Netflix
  pair, 2.4e-5 at 3840x2160 and 1.4e-4 on the 1080p checkerboards.

### `motion_cuda` and `motion_v2_cuda`

The `motion` / `motion2` / `motion3` CUDA outputs compute the CPU's order
since [ADR-1372](../../adr/1372-cuda-motion-diff-first-pipeline.md), in the
kernel `motion_v2_cuda` already used; see
[CPU parity: motion, options and tiny frames](#cpu-parity-motion-options-and-tiny-frames).
Parity holds under the non-default `motion_fps_weight` other than 1.0 and
`motion_moving_average = true` paths after
[ADR-0358](../../adr/0358-cuda-motion-race-and-precision-fixes.md) fixed the
host-side post-processing:

- `motion2_score` applies `MIN(score * motion_fps_weight, motion_max_val)`,
  mirroring the CPU reference at `integer_motion.c:563`.
- The moving-average guard in `motion3_postprocess_cuda` skips averaging at
  framework-collect index 1 to match `integer_motion.c:523`'s
  `index > minimum_past_frames_needed` rule.

Before ADR-1372 the outputs agreed with the CPU fixed-point path at
`places = 4` under default settings on the Netflix
`src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv` pair (0 of 144
mismatches), but not bit for bit: `motion_cuda` blurred each frame and
differenced the blurred frames, where the CPU blurs the difference, and the
two orders round differently (about 1.3e-5 on that pair).

### SpEED (`speed_chroma`, `speed_temporal`)

The GPU SpEED extractors are bit-identical to the CPU reference, verified on
an RTX 4090 via `test_cuda_speed_chroma_parity` and
`test_cuda_speed_temporal_parity`. Since ADR-1380 the CUDA SpEED twins
reproduce the CPU's fp32 arithmetic step for step; see
[the device-resident section](#cambi-and-speed-run-entirely-on-the-device-adr-1379-adr-1380).

!!! note
    The first GPU SpEED fix was a score correction, not just a tolerance
    statement. If you recorded GPU SpEED numbers before it, re-extract them.

Before that fix the GPU SpEED kernels computed a per-tile block-local
covariance (instead of the CPU's single global covariance over the
5x5-phase-shifted submatrix) and reused the reference eigenvalue basis for the
distorted path, so GPU SpEED scores were about 7x low (and chroma about 2x
high on the distorted path). See
[research-1120](../../research/1120-gpu-speed-covariance-eigenbasis-correctness-2026-06-20.md).
The same correction was ported to the HIP and SYCL SpEED twins.

## History

Newest first within each topic. These notes record how the current behaviour was
reached; they are kept for traceability.

### Earlier agreement figures (before the exact-twin changes)

The overview table states the current agreement. The figures it replaced,
measured against `--backend cpu` at `--precision max` on an RTX 4090 for the
same fixtures:

| Twin | Difference before the change |
|---|---|
| `speed_chroma` | 13 of 789 outputs differed from a glibc CPU by up to 1.4e-6 (the twin rounded `log2` on the device and the CPU called `log2f`; fixed by ADR-1477, which forms the entropies on the host) |
| `ssimulacra2` | 7.3e-11 (the terms were added in a tree; ADR-1433) |
| `ssim` | 1.1e-11 (the terms were added per block; ADR-1424) |
| `adm` | 2.1e-7 (the host computed its own CSF weights; ADR-1416) |
| `float_adm` | 1.3e-5 (the angle test's threshold was associated differently; ADR-1420) |
| `ciede` | 1.1e-5 (the kernel computed in fp32; ADR-1426), then 62 of 113 frames identical and the rest within 1.4e-11 (ADR-1426), then 127 of 180 identical and at most 5.2e-12 (ADR-1467, GCC 16.2.1 CPU; 113 frames and 2.0e-11 before) |
| `float_vif` | 3.8e-5 (the kernel's tap table was not the one the CPU computes; ADR-1412) |
| `float_ms_ssim` `enable_lcs` outputs | up to 1.3e-6 |

### 2026-10: `float_moment_cuda` in two steps

The 16-bit float-square change (ADR-1453, 2026-10-02) came first; the sum past
$2^{53}$ units (ADR-1497, 2026-10-03) followed. The "before" columns of the
`float_moment` table above belong to each step.

### 2026-10-02: ADR-1464 and ADR-1465 raster-order sums

Before ADR-1464, `float_ssim_cuda` added the per-window SSIM terms in
blocks. A search over noise frames found two in 31 million whose mean rounded
to the next `float`: the CPU reports -4.222829943500983e-07 and the twin
reported -4.222829659283889e-07.

Before ADR-1465 the same held for `float_ms_ssim_cuda`: four of 8.3 million
noise frames, one of them changing the score in its tenth digit
(0.06886243290982871 on the CPU, 0.0688624330951202 on the twin).

The pre-change versions of the "Exact twins declared as a group" section said
these two were "exact up to one rounding, a few in a million could differ";
ADR-1464 and ADR-1465 closed that gap.

### 2026-09-30: CAMBI option and semantics mismatch

`cambi_cuda` ignored `cambi_high_res_speedup` and mis-mirrored two
kernel-level semantics (spatial-mask edge padding, `filter_mode` border rows)
before branch `fix/gpu-cambi-parity-drift`, which cost 1.76e-2 pooled `vmaf`
on a 1080p pair.

### 2026-09-17: CAMBI reads its device buffers in one transfer (superseded)

Superseded by the device-resident pipeline of
[ADR-1379](../../adr/1379-cuda-cambi-device-resident-pipeline.md), which reads
back 88 bytes per frame. Kept for the measurements.

`integer_cambi_cuda.c` used to copy the decimated image and mask back to the
host **one row at a time**, with a blocking `cuMemcpyDtoH` per row per scale.
At 1080p that is 2,160 driver round trips for scale 0 alone and roughly 4,200
per frame across the five scales.

Measured per extractor on an RTX 4090 over 48 frames of 1080p, before the fix:

| extractor | phase | time |
| --- | --- | ---: |
| `cambi_cuda` | submit | 0.602 s |
| `speed_chroma_cuda` | extract | 0.239 s |
| `adm_cuda` | submit | 0.003 s |
| `motion_cuda` | submit | 0.000 s |

The copies, not the kernels, were the pipeline: 0.602 s of a 1.03 s run. The
upload and both readbacks are now single strided `cuMemcpy2DAsync` transfers
enqueued on the stream with one stall for both — the host picture's stride and
the packed device buffer's differ, which is what the pitch fields are for.
`cambi_cuda` submit drops to **0.142 s** and the default-model CUDA run goes
from **47 to 67 fps** at 1080p. Scores are unchanged; the two CAMBI parity
gates and the rest of the GPU suite pass.

**If you add a GPU twin that reads a plane back**, copy it in one transfer.
A per-row loop looks harmless and is the single most expensive thing this
pipeline has done.

### 2026-09-06: SpEED-chroma 4K failure and `psnr_hvs_cuda` default

Before the ADR-1202 fix, `speed_chroma_u`, `speed_chroma_v` and
`speed_chroma_uv` came back as exactly `0.000000` from the CUDA backend at
3840x2160 and above, and the run still exited 0. With the default model that
moved pooled VMAF by about 3.4 points against the CPU score. 1920x1080 and
2560x1440 were correct; the threshold is 256 linear systems, which a 4K
chroma plane exceeds.

Before the ADR-1203 fix the CUDA twin alone defaulted `psnr_hvs`'s
`enable_chroma` to `false`. `--feature psnr_hvs_cuda` therefore returned the
luma-only number under the `psnr_hvs` name and emitted no `psnr_hvs_cb` /
`psnr_hvs_cr` at all, despite both being listed in its `provided_features[]`
and in [the feature table](../../metrics/features.md). On a 960x540 pair that
put CUDA about 4% away from CPU (41.4866616015 against 41.7803055708), which
is what `test_cuda_psnr_hvs_parity` had been failing on.

### 2026-05-29: SSIM vert_combine kernel performance (ADR-0754)

`calculate_ssim_vert_combine` is the pass-2 (vertical 11-tap + SSIM combine)
kernel in the `float_ssim_cuda` extractor. Three optimizations applied in
[ADR-0754](../../adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md):

1. **`__launch_bounds__(128)`** — constrains register budget to the actual
   128-thread (16×8) launch configuration. Minimum-form hint with no
   `min_blocks` argument (conservative, zero risk of regression).

2. **`__ldg()` on the 5×11 = 55 inner-loop loads** — the five intermediate
   float buffers (h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp) are
   written once by the horizontal pass and never aliased in the vertical
   pass. Extracting `const float *__restrict__` pointers from the
   `VmafCudaBuffer` struct arguments before the inner loop makes the
   alias-free invariant visible to the compiler, enabling `__ldg()` to
   route all 55 loads through the L1 read-only cache. Expected benefit at
   ≥ 1080p where the combined 5-plane footprint exceeds L2 capacity.

3. **Pinned-host memory leak fix** — at the time,
   `vmaf_cuda_kernel_readback_free`
   NULLed `rb->host_pinned` without freeing it, and `close_fex_cuda` freed the
   pointer afterwards, verified with
   `compute-sanitizer --tool memcheck --leak-check full`. This is superseded:
   `vmaf_cuda_kernel_readback_free` now frees the pinned host buffer itself
   (see [kernel scaffolding](../kernel-scaffolding.md)), and a caller must not
   free `rb->host_pinned` again.

Live ncu A/B numbers were not measured; static analysis predicted behaviour
analogous to the VIF filter1d `__ldg()` pattern at 1080p+.

ncu reproducer:

```bash
ncu --kernel-name calculate_ssim_vert_combine \
    --section MemoryWorkloadAnalysis --section LaunchStats \
    build/tools/vmaf \
      --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
      --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
      --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
      --feature float_ssim --backend cuda
```

### 2026-05-28: VIF filter1d horizontal kernel performance (ADR-0743)

`filter1d_8_horizontal_kernel_2_17_9` is the scale-0 8-bit 17-tap horizontal
convolution pass and accounts for 35.3% of VIF self-time on RTX 4090 / CUDA
13.3.

Two ncu-driven optimizations were applied:

1. **`__launch_bounds__(128, 10)`** on the kernel: reduces registers 56 → 48
   per thread on sm_89 (RTX 4090), lifting theoretical occupancy 75% → 83.3%.
   At production resolutions (≥ 1080p) the higher block count per SM improves
   latency hiding. At 576×324 the workload is wave-limited (< 1 wave / 128 SMs)
   and the gain is invisible in achieved occupancy but causes no regression.

2. **`__ldg()` on the 7 read-only tmp-channel loads** in the smem-fill phase:
   routes these loads through the read-only L1 (texture) cache. Beneficial at
   ≥ 1080p where the combined tmp footprint (7 channels × stride × height)
   exceeds
   the 50 MB L2 capacity.

`val_per_thread=4` was evaluated but rejected: smem grows 7644 → 14812 B/block,
making the kernel smem-limited at 37.5% occupancy vs 62.5% for the retained
vpt=2 path.

Correctness: CUDA-optimized scores agree with the CPU reference within
ADR-0214 places=4 tolerance (max absolute delta: 0.000010 per frame).

ncu reproducer (see research digest):

```bash
ncu -k 'filter1d_8_horizontal_kernel_2_17_9' --set basic --csv \
    build/tools/vmaf -r ref.yuv -d dis.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 --backend cuda
```

See [ADR-0743](../../adr/0743-cuda-vif-filter1d-ncu-driven-perf.md) and
[Research-0743](../../research/research-0743-cuda-vif-filter1d-perf-impl.md).

### Integer SSIM `extern "C"` sweep (ADR-0747)

A full audit
of all 24 `.cu` kernel files confirmed that `integer_ssim/integer_ssim_score.cu`
was the only file with `__global__` kernels referenced by
`cuModuleGetFunction` but not wrapped in `extern "C"`.  This caused
`--feature ssim --backend cuda` to silently return `-EINVAL` from
`init_fex_cuda` (the driver returned `CUDA_ERROR_NOT_FOUND` for all
three kernel names) since the file was introduced.  Fixed by
wrapping the three entry points in `extern "C" { }`.  A CI script
(`scripts/dev/check-cuda-extern-c.sh`) prevents recurrence.
The analogous bug in `ssim_score.cu` was fixed earlier in PR #77.
