<!-- markdownlint-disable MD060 -->
# Motion

Motion measures temporal activity in the **reference** stream by computing the
mean absolute difference (MAD) between consecutive Gaussian-blurred luma frames.
It is used as a VMAF model feature to weight distortion scores: scenes with high
motion are treated differently from static content. A frozen reference scores
0; larger values mean more temporal activity.

Three extractor variants are registered:

| Extractor name   | Algorithm                      | Temporal? | Output keys in the JSON log |
|------------------|-------------------------------|-----------|-----------------------------|
| `motion`         | Integer fixed-point (Motion2) | Yes       | `integer_motion2`, `integer_motion3`, `VMAF_integer_feature_motion_sad_score` (`integer_motion` with `debug=true`) |
| `motion_v2`      | Integer pipelined (Motion2 v2)| Yes       | `VMAF_integer_feature_motion_v2_sad_score`, `VMAF_integer_feature_motion2_v2_score`, `VMAF_integer_feature_motion3_v2_score` |
| `float_motion`   | Floating-point (Motion2)      | Yes       | `motion2`, `motion3`, `motion` (the last is on by default) |

## How to run

Options follow the feature name after the first `=`, separated by `:`.

```bash
# Integer motion (default)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature motion --output /dev/stdout

# Integer motion with an FPS weight
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature motion=motion_fps_weight=0.8 \
    --output /dev/stdout

# Float motion with the 3-tap filter
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature float_motion=motion_filter_size=3 \
    --output /dev/stdout
```

An option with a non-default value is appended to the output key as its alias
and value, for example `integer_motion2_mfw_0.8` for `motion_fps_weight=0.8`.
The CLI also accepts `integer_motion` and `integer_motion2` as aliases of
`motion` and `motion_v2`.

---

## `motion` extractor (integer fixed-point)

Registered name: `motion` (`VmafFeatureExtractor vmaf_fex_integer_motion`).

The extractor blurs the difference of two reference luma frames with a
separable 5-tap Gaussian filter and sums the absolute values (the frame's SAD).
`motion2` of a frame is the smaller of the SADs on both sides of it. By default
it looks one frame back and one frame ahead (the three-frame window); with
`motion_five_frame_window` it reads three frames back and one ahead
([Five-frame window](#five-frame-window)).

### Output features

| Feature name                             | Description                                     | Condition         |
|------------------------------------------|-------------------------------------------------|-------------------|
| `VMAF_integer_feature_motion2_score`     | Motion2 score (shipped VMAF model input)        | Always            |
| `VMAF_integer_feature_motion3_score`     | Perceptually blended motion score               | Always            |
| `VMAF_integer_feature_motion_score`      | Raw (unfixed) motion score for back-compat      | `debug=true` only |
| `VMAF_integer_feature_motion_sad_score`  | The frame's SAD score: weighted by `motion_fps_weight`, capped at `motion_max_val`; the value `motion2` and `motion3` are derived from | Always |

Frame 0 always emits `motion2_score = 0.0`. The output range is
`[0, motion_max_val]` (default 10 000), with no inherent upper bound; larger
values indicate more temporal activity.

### Options

| Option                    | Alias    | Type   | Default   | Range         | Effect                                                                 |
|---------------------------|----------|--------|-----------|---------------|------------------------------------------------------------------------|
| `debug`                   | none     | bool   | `false`   | n/a           | Emit `motion_score` (legacy unfixed variant) alongside `motion2_score`; `motion_cuda`, `motion_sycl` and `motion_hip` default to false too |
| `motion_force_zero`       | `force_0`| bool   | `false`   | n/a           | Override all emitted scores to `0.0`; used for deterministic fixtures  |
| `motion_fps_weight`       | `mfw`    | double | `1.0`     | `0.0–5.0`     | Multiplicative FPS-aware correction applied before clamping            |
| `motion_blend_factor`     | `mbf`    | double | `1.0`     | `0.0–1.0`     | Blend factor for `motion3_score`                                       |
| `motion_blend_offset`     | `mbo`    | double | `40.0`    | `0.0–1000.0`  | Score offset at which blending begins for `motion3_score`              |
| `motion_max_val`          | `mmxv`   | double | `10000.0` | `0.0–10000.0` | Upper clamp applied to emitted scores                                  |
| `motion_five_frame_window`| `mffw`   | bool   | `false`   | n/a           | Take each SAD against the frame two back instead of the previous one ([Five-frame window](#five-frame-window)); also on `motion_cuda`, `motion_sycl` and `motion_hip` |
| `motion_moving_average`   | `mma`    | bool   | `false`   | n/a           | Apply a two-frame moving average to `motion3_score`                    |

!!! warning "`motion_fps_weight` is applied exactly once"
    The weight scales the SAD-derived score once, in `extract()`, on the CPU and
    on every GPU twin. Up to and including v3.2.1 the CUDA, SYCL and HIP twins
    applied it again, so `motion3_score` carried the weight squared.

The CPU reference scales the SAD-derived score by `motion_fps_weight` in
`extract()`, stores the weighted value as `motion_sad_score`, and blends that
already-weighted value into `motion2_score` / `motion3_score` without touching
the weight again. The old GPU behaviour showed whenever the option was set away
from `1.0` (`motion2_score` was always correct). Fixed per
[ADR-1216](../adr/1216-gpu-motion3-fps-weight-applied-once.md); the
`test_<backend>_motion3_parity` tests now pin `motion_fps_weight = 0.6` and
assert CPU/GPU parity on the derived `integer_motion3_mfw_0.6` key, so the
default value can no longer hide a squared weight.

### Backends

| Backend | Extractor | Source | Arithmetic vs CPU | Evidence (fragments in `scripts/ci/exact_twins.d/`) |
|---|---|---|---|---|
| Scalar C | `motion` | `integer_motion.c` | Reference | n/a |
| AVX2, AVX-512, NEON | `motion` | `x86/motion_avx2.c`, `x86/motion_avx512.c`, `arm64/motion_neon.c` | Same bits | n/a |
| CUDA | `motion_cuda` | `feature/cuda/integer_motion_cuda.c` | Exact ([ADR-1372](../adr/1372-cuda-motion-diff-first-pipeline.md), ADR-1457) | `motion.cuda`, `motion_debug.cuda`, `motion_mffw.cuda` |
| SYCL | `motion_sycl` | `feature/sycl/integer_motion_sycl.cpp` | Exact ([ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), ADR-1451) | `motion.sycl`, `motion_debug.sycl`, `motion_mffw.sycl` |
| HIP | `motion_hip` | `feature/hip/integer_motion_hip.c` | Exact ([ADR-1377](../adr/1377-hip-motion-diff-first.md), ADR-1437) | `motion.hip`, `motion_debug.hip`, `motion_mffw.hip` |
| Metal | `integer_motion_metal` | `feature/metal/integer_motion_metal.mm` | Not exact (see below) | none |

How the twins match the CPU:

- **CPU.** It computes the SAD of the blurred difference of the two frames,
  rounding after the vertical and after the horizontal filter pass.
- **CUDA, SYCL, HIP.** Each does the same. The CUDA twin runs the kernel
  `motion_v2_cuda` already used, and its debug `motion` score is the CPU's
  (weighted by `motion_fps_weight`, capped at `motion_max_val`).
- **Metal.** The twin still blurs each frame and compares the blurred frames,
  which rounds differently. The SYCL twin did the same until 2026-09-29 and its
  `motion2_score` was up to 2.0e-4 off on 17x17 frames and 1.3e-5 on the Netflix
  576x324 pair; expect the same from the Metal twin, which has not been measured
  yet (its row in [`state.md`](../state.md)).

What the twins emit:

| Backend | `motion2`, `motion3` (3-frame window) | `motion_sad_score` | `motion_force_zero` | Five-frame window |
|---|---|---|---|---|
| CUDA | Yes | Yes, since 2026-10-02 | Yes | Yes (ADR-1491) |
| SYCL | Yes | Yes, since 2026-10-02 | Yes, since 2026-10-02 | Yes (ADR-1491) |
| HIP | Yes | Yes | Yes | Yes (ADR-1491) |
| Metal | `motion` and `motion2` only (no `motion3`) | No (`T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02` in [`state.md`](../state.md)) | not recorded | No: the CPU extractor computes it |

The parity gate compares the SAD score in its `motion` and `motion_debug`
cells, so a twin without it fails the cell.

!!! note "`motion_sad_score` was missing on CUDA and SYCL before 2026-10-02"
    A `--backend cuda` or `--backend sycl` run lacked
    `VMAF_integer_feature_motion_sad_score` before that date
    (`T-CUDA-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02`).

!!! warning "`motion_force_zero` was ignored on `motion_sycl` before 2026-10-02"
    The twin declared the option and ignored it: with `motion_force_zero=true`
    it returned the measured `motion2` / `motion3` under the `_force_0` names
    where the CPU returns 0 (`T-SYCL-MOTION-FORCE-ZERO-IGNORED-2026-10-02`).
    That reached a shipped model: `model/other_models/vmaf_v0.6.1mfz.json` sets
    the option, and on `--backend sycl` it scored the Netflix 576x324 pair
    76.668 where the CPU scores 72.321. Both give 72.321 now.

---

## `motion_v2` extractor (pipelined integer)

Registered name: `motion_v2`
(`VmafFeatureExtractor vmaf_fex_integer_motion_v2`).

A pipelined re-implementation that exploits the linearity of the blur kernel:
`SAD(blur(f[N-1]), blur(f[N])) == sum(|blur(f[N-1] - f[N])|)`. The frame
difference, blur, and absolute-sum are fused into a single row-at-a-time
pipeline requiring only one scratch row. Per-frame blurred-state storage is
eliminated.

### Output features

| Feature name                                  | Description                                       | Condition  |
|-----------------------------------------------|---------------------------------------------------|------------|
| `VMAF_integer_feature_motion_v2_sad_score`    | Per-frame sum of absolute blurred differences     | Always     |
| `VMAF_integer_feature_motion2_v2_score`       | Motion2-equivalent smoothed score                 | Always     |
| `VMAF_integer_feature_motion3_v2_score`       | Perceptually blended + clipped score (optional 2-frame moving average) | Always |

`motion3_v2_score` is the `motion_v2` analogue of motion v1's `motion3_score`:
a per-frame `motion_blend(motion2, motion_blend_factor, motion_blend_offset)`
clipped to `motion_max_val`, optionally smoothed by a two-frame moving average
(`motion_moving_average=true`). It is emitted host-side in the extractor's
end-of-stream flush. The output range is `[0, motion_max_val]`, in the same
units as `motion`.

### Options

| Option               | Alias     | Type   | Default   | Range         | Effect                                    |
|----------------------|-----------|--------|-----------|---------------|-------------------------------------------|
| `motion_force_zero`  | `force_0` | bool   | `false`   | n/a           | Override all scores to `0.0`              |
| `motion_fps_weight`  | `mfw`     | double | `1.0`     | `0.0–5.0`     | FPS-aware multiplicative correction       |
| `motion_blend_factor`| `mbf`     | double | `1.0`     | `0.0–1.0`     | Blend factor for motion3-style score      |
| `motion_blend_offset`| `mbo`     | double | `40.0`    | `0.0–1000.0`  | Blend offset                              |
| `motion_max_val`     | `mmxv`    | double | `10000.0` | `0.0–10000.0` | Upper clamp                               |
| `motion_five_frame_window` | `mffw` | bool | `false` | n/a           | SAD against the frame two back, as on `motion` ([Five-frame window](#five-frame-window)); also on `motion_v2_cuda`, `motion_v2_sycl` and `motion_v2_hip` (ADR-1491), on the CPU for `--backend metal` |
| `motion_moving_average` | `mma` | bool   | `false`   | n/a           | Two-frame moving average                  |

### Backends

| Backend | Extractor | Source | vs CPU | Evidence (fragments) |
|---|---|---|---|---|
| Scalar C | `motion_v2` | `integer_motion_v2.c` | Reference | n/a |
| AVX2, AVX-512, NEON | `motion_v2` | `x86/motion_v2_avx2.c`, `x86/motion_v2_avx512.c`, `arm64/motion_v2_neon.c` (ADR-0145, bit-exact) | Same bits | n/a |
| CUDA | `motion_v2_cuda` | `feature/cuda/integer_motion_v2_cuda.c` | Exact (ADR-1372, ADR-1457) | `motion_v2.cuda`, `motion_v2_mffw.cuda` |
| SYCL | `motion_v2_sycl` | `feature/sycl/integer_motion_v2_sycl.cpp` | Exact (ADR-1371, ADR-1451) | `motion_v2.sycl`, `motion_v2_mffw.sycl` |
| HIP | `motion_v2_hip` | `feature/hip/integer_motion_v2_hip.c` | Exact (ADR-1377, ADR-1437) | `motion_v2.hip`, `motion_v2_mffw.hip` |
| Metal | `motion_v2_metal` | `feature/metal/integer_motion_v2_metal.mm` | Bit-exact on the cross-backend gate fixture | none |

All four GPU twins emit `motion3_v2_score` and accept the `motion_blend_factor`,
`motion_blend_offset`, `motion_max_val` and `motion_moving_average` options
([ADR-1108](../adr/1108-cuda-motion-v2-motion3-emission.md)):

- **Where it runs.** The score is computed host-side over the kernel's SAD
  scores via the shared `motion_blend_tools.h` helper. The CUDA twin landed
  first (#909); the SYCL, HIP and Metal twins mirror its `flush_fex`
  post-process byte for byte.
- **Parity.** It is bit-exact at default options (`max_abs_diff = 0.0` on the
  Netflix `src01` 576x324 pair, 48 frames), verified by the per-backend
  `test_<backend>_motion_v2_parity` tests, which assert `motion_v2_sad`,
  `motion2_v2` and `motion3_v2` at `places=4` and skip cleanly when the backend
  device is absent.
- **Mirror rule.** The host-side post-process is identical across all GPU
  twins, so any change to the CPU `motion_v2` flush blend, clip, seed or
  moving-average logic must be mirrored into all four in the same PR.
- **Edge formula.** All GPU kernels use the CPU `integer_motion_v2.c::mirror`
  high-edge formula (`2 * size - idx - 2`).

!!! note "`motion_fps_weight` and `motion_max_val` on `motion_v2`"
    The CPU reference stores `MIN(sad * motion_fps_weight, motion_max_val)` as
    `motion_v2_sad_score` and derives `motion2_v2` / `motion3_v2` from it; a
    one-frame input still gets `motion2_v2 = motion3_v2 = 0`. `motion_v2_cuda`
    and `motion_v2_hip` do the same
    ([ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md),
    [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)), and so does
    `motion_v2_sycl`. The Metal twin stores the *raw* SAD, applies
    `motion_fps_weight` in the host-side flush without the `motion_max_val` cap
    on `motion2_v2`, and emits no `motion2_v2` / `motion3_v2` for a one-frame
    input. The paths agree at the default `motion_fps_weight = 1.0` and
    `motion_max_val = 10000`
    (`T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30` in
    [`state.md`](../state.md)).

---

## `float_motion` extractor (floating-point)

Registered name: `float_motion` (`VmafFeatureExtractor vmaf_fex_float_motion`).

Floating-point twin of `motion` using `float` arithmetic throughout. It
provides additional options for chroma channels and a half-resolution scale-1
SAD term. The output range is `[0, motion_max_val]`, with the same semantics as
`motion`.

### Output features

| Feature name                          | Description                                | Condition         |
|---------------------------------------|--------------------------------------------|-------------------|
| `VMAF_feature_motion2_score`          | Motion2 score                              | Always            |
| `VMAF_feature_motion3_score`          | Perceptually blended score                 | Always            |
| `VMAF_feature_motion_score`           | Raw (unfixed) motion score                 | `debug=true` (the default) |

### Options

| Option               | Alias     | Type   | Default   | Range         | Effect                                                                    |
|----------------------|-----------|--------|-----------|---------------|---------------------------------------------------------------------------|
| `debug`              | none      | bool   | `true`    | n/a           | Emit `motion_score` alongside `motion2_score`                             |
| `motion_force_zero`  | `force_0` | bool   | `false`   | n/a           | Override all scores to `0.0`                                              |
| `motion_fps_weight`  | `mfw`     | double | `1.0`     | `0.0–5.0`     | FPS-aware multiplicative correction                                       |
| `motion_blend_factor`| `mbf`     | double | `1.0`     | `0.0–1.0`     | Blend factor for `motion3_score`                                          |
| `motion_blend_offset`| `mbo`     | double | `40.0`    | `0.0–1000.0`  | Blend offset for `motion3_score`                                          |
| `motion_add_scale1`  | `mdc`     | bool   | `false`   | n/a           | Add half-resolution SAD term on top of the full-resolution SAD of each plane (CPU and `float_motion_hip`) |
| `motion_add_uv`      | `mau`     | bool   | `false`   | n/a           | Sum U and V plane SADs into the score (CPU and `float_motion_hip`)        |
| `motion_filter_size` | `mfs`     | int    | `5`       | `0–9`         | Blur filter: `3` = 3-tap, `1` = no blur, any other value = the 5-tap Motion2 filter (CPU and `float_motion_hip`) |
| `motion_max_val`     | `mmxv`    | double | `10000.0` | `0.0–10000.0` | Upper clamp applied to emitted scores                                     |

`motion_add_scale1`, `motion_add_uv`, `motion_filter_size` and `motion_max_val`
were ported from Netflix/vmaf commit `b949cebf`. With default settings the
output is bit-identical to the pre-port baseline on the Y-plane SIMD fast path.
`motion_add_uv` needs a pixel format with chroma planes; 4:0:0 input is refused
at init.

### Backends

| Backend | Extractor | Source | vs CPU | motion3 | Options accepted | Evidence (fragment) |
|---|---|---|---|---|---|---|
| Scalar C, AVX2, AVX-512, NEON | `float_motion` | `float_motion.c`, `x86/float_motion_avx2.c`, `x86/float_motion_avx512.c`, `arm64/float_motion_neon.c` | Reference | Yes | All | n/a |
| CUDA | `float_motion_cuda` | `feature/cuda/float_motion_cuda.c` (ADR-0196) | Exact ([ADR-1409](../adr/1409-float-motion-twins-cpu-float-sum.md)) | Yes | `debug`, `motion_force_zero`, `motion_fps_weight`, `motion_max_val`, `motion_blend_factor`, `motion_blend_offset` | `float_motion.cuda` |
| SYCL | `float_motion_sycl` | `feature/sycl/float_motion_sycl.cpp` (ADR-0196) | Exact ([ADR-1411](../adr/1411-sycl-float-motion-cpu-float-sum.md)) | Yes, since 2026-10-03 | As CUDA | `float_motion.sycl` |
| HIP | `float_motion_hip` | `feature/hip/float_motion_hip.c` (ADR-0273) | Exact ([ADR-1419](../adr/1419-hip-float-motion-cpu-float-sum.md)) | Yes ([ADR-1404](../adr/1404-hip-float-motion-motion3-and-options.md)) | The whole CPU table, including `motion_add_scale1`, `motion_add_uv`, `motion_filter_size` | `float_motion.hip` |
| Metal | `float_motion_metal` | `feature/metal/float_motion_metal.mm` | Not exact (per-block sum) | No (`motion` and `motion2` only; `T-GPU-FLOAT-MOTION3-MISSING-2026-09-30` in [`state.md`](../state.md)) | Not `motion_max_val`; see below | none |

The CUDA, SYCL and Metal twins do not declare `motion_add_scale1`,
`motion_add_uv` or `motion_filter_size`, so a request with one of them is
computed on the CPU
([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)). On Metal a
`motion_max_val` setting keeps `float_motion` on the CPU as well, and the debug
`motion` score is emitted without the fps weight.

### Why the float twins can be exact

The score depends on the order of the additions: the CPU adds the absolute
differences of a row into one `float`, the row sums into a second one, and
divides in `float`, and those running sums round at every step. A twin that
sums per block instead is closer to the exact mean and differs from the CPU in
the low digits.

- **CUDA** ([ADR-1409](../adr/1409-float-motion-twins-cpu-float-sum.md)). The
  twin adds each row on the device in the CPU's order and the rows on the host.
  It returns `motion`, `motion2` and `motion3` bit for bit at `--precision max`,
  at every frame size and sample depth. Before the change it was off by 3e-6 on
  the Netflix 576x324 pair, 2.4e-5 at 3840x2160 and 1.4e-4 on 1920x1080
  checkerboards.
- **SYCL** ([ADR-1411](../adr/1411-sycl-float-motion-cpu-float-sum.md)). One
  work-item per row adds the row on the device and the host adds the rows.
  `motion` and `motion2` equal the CPU's on every frame (measured on an Arc
  A380: the Netflix pair, both 1080p checkerboard pairs, 200 frames of BBB
  3840x2160, and the Netflix pair at 10, 12 and 16 bits). The second pass over
  the blurred planes costs 0.38 ms per 3840x2160 frame on that GPU (3.85 to
  4.23 ms).
- **HIP** ([ADR-1419](../adr/1419-hip-float-motion-cpu-float-sum.md)). The twin
  stores its differences transposed and adds each row in the CPU's order, so it
  is exact too (`float_motion.hip`: 1617 of 1617 values over seven option sets
  on a gfx1036).

### `motion3` and the options on the twins

- **CUDA.** `motion3` is the CPU's blend of `motion2` (`motion_fps_weight`, then
  the `motion_blend_factor` / `motion_blend_offset` blend, then the
  `motion_max_val` cap), with frame 0 taken from the first SAD and the last
  frame from the flush. It takes both blend options (aliases `mbf` / `mbo`).
- **HIP.** The same, and the whole CPU option table
  ([ADR-1404](../adr/1404-hip-float-motion-motion3-and-options.md)): the filter
  is a kernel argument, the scale-1 term a second kernel, and `motion_add_uv`
  runs both on the U and V planes.
- **SYCL.** `motion3` and both blend options since 2026-10-03. Its `motion3`
  is the CPU's bit for bit, because its SAD already is (ADR-1411) and the blend
  is the CPU's host arithmetic.
- **`motion_max_val` and `motion_fps_weight`.** `float_motion_sycl`,
  `float_motion_cuda` and `float_motion_hip` take `motion_max_val` (alias
  `mmxv`) and, like the CPU, scale every score they emit (the debug `motion`
  too) by `motion_fps_weight` before capping it at `motion_max_val`
  ([ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md),
  [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md),
  [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)).
- **`motion_force_zero`.** `float_motion_cuda` and `motion_cuda` publish zeros
  from the first frame; `motion_hip` and `float_motion_hip` do as well.

Measured for the SYCL `motion3` on an Arc A380 at `--precision max` against
`--backend cpu`: every `motion`, `motion2` and `motion3` value is identical on
the Netflix 576x324 pair (48 frames), both 1920x1080 checkerboard pairs and 200
frames of BBB 3840x2160. That holds at the default options and with
`motion_blend_factor=0.5:motion_blend_offset=2`, and on the small fixtures also
with `motion_fps_weight=2:motion_max_val=4`, with all four score options
together, with `motion_force_zero` and for a one-frame input (`motion3` = 0).

```bash
# On float_motion_sycl with a motion cap
# (keys become motion_mmxv_4 / motion2_mmxv_4 / motion3_mmxv_4)
vmaf --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend sycl --no_prediction \
    --feature float_motion=motion_max_val=4 --output /dev/stdout

# On float_motion_sycl with the motion3 blend
# (keys become motion_mbf_0.5_mbo_2 / motion2_mbf_0.5_mbo_2 / motion3_mbf_0.5_mbo_2)
vmaf --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend sycl --no_prediction \
    --feature float_motion=motion_blend_factor=0.5:motion_blend_offset=2 \
    --output /dev/stdout
```

---

## Five-frame window

`motion_five_frame_window=true` is the motion feature of the four
`vmaf_v1.0.16_hfr_*` models ([VMAF v1 models](../models/v1.md)):
`vmaf_v1.0.16_hfr_3d0h`, `vmaf_v1.0.16_hfr_3d0h_2160`,
`vmaf_v1.0.16_hfr_5d0h` and `vmaf_v1.0.16_hfr_1d5h_2160`. They set it together
with `motion_moving_average=true`. It is Netflix's option (`a2b59b77`,
`a4a1492d`), ported with its arithmetic unchanged
([ADR-1478](../adr/1478-motion-five-frame-window-port.md)); the scores equal
upstream Netflix `9e48141b` bit for bit. No other shipped model sets it.

### What it changes

With the option, for a sequence of `N` frames:

| Score | Three-frame window (default) | Five-frame window |
|---|---|---|
| SAD of frame `n` | frames `n-1` and `n`; 0 for `n = 0` | frames `n-2` and `n`; 0 for `n < 2` |
| `motion2` of frame `n` | `min(SAD[n], SAD[n+1])`; `SAD[n]` for the last frame; 0 for `n = 0` | `min(SAD[n-1], SAD[n+1])`; `SAD[3]` for `n = 2`; `SAD[n]` for the last frame; 0 for `n < 2` |
| `motion3` of the frames without a SAD | the blended `SAD[1]` | the blended `SAD[2]` (frames 0 and 1) |

So `motion2` of frame `n` reads the frames `n-3`, `n-1` and `n+1`, a span of
five frames (it does not read frame `n` itself). `motion3` is derived from
`motion2` as in the default mode (`motion_blend_factor`, `motion_blend_offset`,
`motion_max_val`, `motion_moving_average`). A sequence of one or two frames has
no SAD and every score is 0.

### How to run

```bash
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction \
    --feature motion=motion_five_frame_window=true:motion_moving_average=true \
    --output /dev/stdout
```

The scores are written under names that carry the option aliases:
`integer_motion2_mffw_mma`, `integer_motion3_mffw_mma` and
`VMAF_integer_feature_motion_sad_score_mffw_mma`.

### What the framework keeps

For the extra frame the framework keeps the reference pictures of the two
frames before the current one (`prev_ref`, `prev_prev_ref`):

- **When.** Only while an extractor with the option is registered; otherwise it
  keeps the frame before the current one alone, as without the port. Netflix
  keeps both in every run; the fork's narrower rule changes which pictures stay
  allocated, not a score
  ([ADR-1478](../adr/1478-motion-five-frame-window-port.md)).
- **Pool size.** A preallocated picture pool then needs at least four
  pictures: the current pair and those two.
- **Error.** A smaller pool is refused with `-EINVAL` when the extractor is
  registered or the pool is allocated, whichever comes second, instead of
  stalling on the third frame ([C API](../api/index.md)).

### GPU twins

`motion_cuda`, `motion_sycl` and `motion_hip` compute the five-frame window
([ADR-1491](../adr/1491-gpu-motion-five-frame-window.md)), and so do
`motion_v2_cuda`, `motion_v2_sycl` and `motion_v2_hip`:

- They keep the frame two back on the device, take the SAD against it with the
  kernel of the three-frame window, and derive `motion2` / `motion3` with the
  CPU's own function when the last frame is in.
- With the option, a twin publishes `motion2` and `motion3` at the end of the
  run, as the CPU does, not frame by frame.
- Their outputs equal the CPU's bit for bit (measured on an RTX 4090, an Arc
  A380 and a gfx1036): 104 of 104 frames (Netflix 576x324 at 8 and 10 bit, a
  1080p checkerboard, BBB 4K 50 frames) on every backend, and the parity gate
  compares the cell `motion_mffw` (and `motion_v2_mffw`) with tolerance 0
  (fragments `motion_mffw.{cuda,hip,sycl}` and
  `motion_v2_mffw.{cuda,hip,sycl}`).
- `motion_metal` and `motion_v2_metal` do not declare the option: on
  `--backend metal` the CPU extractor computes the motion feature of a model or
  `--feature motion` that sets it (not measured, no device).

---

## Input format constraints

All three extractors:

- Accept YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc.
- Operate on the Y (luma) plane only by default. Chroma is only included when
  `motion_add_uv=true` is set on `float_motion`, and only for formats other
  than YUV 4:0:0.
- Require a minimum frame size of 3x3 pixels (5-tap Gaussian minimum
  dimension = filter_radius + 1 = 3). Smaller frames are rejected with `-EINVAL`
  at `init()`. With `motion_filter_size=3` the minimum is 2x2.
- Are temporal extractors: frame 0 always emits `0.0` for all motion scores.

The minimum applies to every plane that is actually convolved, not only to
luma. With `motion_add_uv=true` on `float_motion`, the U and V planes are
blurred at their *subsampled* dimensions, so a 4x4 YUV 4:2:0 frame, whose luma
clears the 3x3 floor, presents a 2x2 chroma plane and is rejected. In practice
`motion_add_uv=true` needs at least 5x5 for 4:2:0 (chroma 3x3), 5x3 for 4:2:2,
and 3x3 for 4:4:4. The error message names the plane that failed:

```text
libvmaf ERROR float_motion: chroma plane 2x2 is below the 5-tap filter
minimum 3x3; refusing to avoid out-of-bounds mirror reads
```

## History

- **2026-10-03.** `float_motion_sycl` emits `motion3` and takes
  `motion_blend_factor` / `motion_blend_offset`. Before, a
  `--backend sycl --feature float_motion` run wrote no `motion3` and gave no
  warning, and a request with a blend option ran on the CPU.
- **2026-10-02.** `motion_cuda` and `motion_sycl` emit
  `VMAF_integer_feature_motion_sad_score`; `motion_force_zero` works on
  `motion_sycl` (see the warnings under [Backends](#backends)).
- **2026-09-30, ADR-1372, ADR-1377.** `motion_cuda` and `motion_hip` take the
  diff-first order. On an RTX 4090 the CUDA twin's `motion2_score` and
  `motion3_score` equal the CPU's bit for bit on the Netflix 576x324 pair and
  on 50 frames of a 3840x2160 clip, where the previous order was 1.26e-5 and
  6.9e-5 off (`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29` in
  [`state.md`](../state.md)). On a gfx1036 the HIP twin's scores equal the CPU's
  on every frame of the Netflix pair, where the previous order was 1.26e-5 off
  (`T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29`).
- **2026-09-30, `motion_force_zero` crash.** Before it, `float_motion_cuda` and
  `motion_cuda` crashed on `motion_force_zero`
  (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`), and so did
  `motion_hip` and `float_motion_hip`
  (`T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30`).
- **2026-09-29, ADR-1371.** `motion_sycl` takes the diff-first order.
- **ADR-1166.** Before it, the `float_motion` guard validated luma only, and a
  sub-minimum chroma plane reached the convolution and read out of bounds
  ([Netflix/vmaf#1582](https://github.com/Netflix/vmaf/issues/1582),
  [Netflix/vmaf#1581](https://github.com/Netflix/vmaf/issues/1581)). The same
  guard is now present on the three Metal twins, which had none at all
  ([Netflix/vmaf#1580](https://github.com/Netflix/vmaf/issues/1580)).
- **ADR-1216.** GPU `motion3_score` carried `motion_fps_weight` squared up to
  and including v3.2.1 (see the warning under [Options](#options)).

## Rust implementation

The integer `motion` extractor has a Rust twin, `motion_rust`
([`core/src/rust/feature/motion`](https://github.com/VMAFx/vmafx/tree/master/core/src/rust/feature/motion)),
a port of `integer_motion.c` that returns the C extractor's scores bit for bit:
`VMAF_integer_feature_motion_sad_score`, `motion2` and `motion3` (and `motion`
with `debug`), with every option of the table, including
`motion_five_frame_window` and `motion_moving_average`. It is built with
`-Denable_rust_features=true` and selected with `VMAF_FEATURE_IMPL=rust` (every
`motion` of the run, including the one a model asks for) or by name with
`--feature motion_rust`; without either the C extractor runs. The JSON
`feature_backends` entry names the twin that ran. Check the twin against the C
extractor on your own clips with:

```bash
python3 scripts/ci/rust_twin_diff.py --vmaf build-rs/tools/vmaf \
  --feature motion --fixtures netflix
```

## See also

- [Features](features.md): full feature extractor reference table
- [ADR-0196](../adr/0196-float-motion-gpu.md): `float_motion` GPU kernels
- [ADR-0219](../adr/0219-motion3-gpu-coverage.md): `motion3` GPU coverage
