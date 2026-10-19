<!-- markdownlint-disable MD060 -->
# HIP twins: what each one computes

Each HIP twin computes the same feature as a CPU extractor on the GPU. This
page lists what every kernel does, the floating-point policy that lets the
twins match the CPU, what exactness cost in frame time, and one section per
twin with its measurements. The overview has the
[coverage table](overview.md#registered-extractors) and the
[agreement table](overview.md#agreement-with-the-cpu).

Unless a section says otherwise, measurements were taken on one gfx1036 (the
integrated GPU of a Ryzen 9 9950X3D, ROCm 7.2.4) at `--precision max` against
`--backend cpu`.

## Kernel notes

Bullets are named after the source file under `core/src/feature/hip/`; the
registered extractor names are in the overview.

### Sums and moments

- **`integer_psnr_hip`**: uint64 atomic-SSE kernel with a warp-64
  `__shfl_down` reduction (`integer_psnr/psnr_score.hip`). Emits `psnr_y`,
  `psnr_cb` and `psnr_cr`.
- **`float_psnr_hip`**: the CPU's float (ref-dis)² per pixel, added as an
  exact integer per block of 256 pixels of one row. The host adds each row's
  exact sum into a `double` in row order, as the CPU does. The score is the
  CPU's bit for bit at 8 to 16 bits, past $2^{53}$ units too
  ([ADR-1440](../../adr/1440-hip-float-psnr-exact-block-sums.md),
  [ADR-1499](../../adr/1499-float-psnr-twins-cpu-row-order.md); see
  [PSNR](../../metrics/psnr.md#float_psnr)). Emits `float_psnr`.
- **`float_moment_hip`**: four uint64 atomic accumulators (ref1st, dis1st,
  ref2nd, dis2nd) with a warp-64 two-uint32-shuffle reduction. The host
  divides by w×h. Emits four `float_moment_*` features, bit-identical to the
  CPU's (see [float_moment_hip](#float_moment_hip)).
- **`integer_psnr_hvs_hip`**: frequency-weighted distortion per 8×8 block,
  porting the CUDA twin and the ADR-1369 native upload design. Emits
  `psnr_hvs` and per-channel variants (see below).

`integer_psnr_hvs_hip` in detail:

- It uploads raw native samples with `vmaf_hip_picture_upload()` and
  converts on the device, which removes host float conversions and unused
  pinned staging allocations.
- Its scores are the CPU extractor's bit for bit
  ([ADR-1401](../../adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). The kernel
  stores the 64 masked coefficient errors of every block and the host adds
  them in the CPU's order, which costs a readback of 256 bytes per block (65
  MB per 3840x2160 frame); see
  [the psnr_hvs page](../../metrics/psnr-hvs.md#gpu-twins).
- It takes the CPU extractor's `enable_chroma` option. With
  `enable_chroma=false` or 4:0:0 input only the luma plane is uploaded and
  scored.

### Motion

- **`float_motion_hip`**: temporal extractor. 5×5 separable Gaussian blur
  and per-block float SAD partials, blur ping-pong (`blur[2]`), a first-frame
  `compute_sad=0` short-circuit, and motion2 / motion3 tail emission in
  `flush()`. Emits `VMAF_feature_motion_score`, `VMAF_feature_motion2_score`
  and `VMAF_feature_motion3_score`, and takes every CPU `float_motion` option;
  see [float_motion_hip options](#float_motion_hip-options).
- **`integer_motion_v2_hip`**: temporal extractor. Raw-pixel ping-pong
  (`pix[2]`), a separable 5-tap Gaussian diff filter with arithmetic
  right-shift (critical for bit-exactness against the CPU; see ADR-0138 and
  ADR-0139 and the PR #587 AVX2 `srlv_epi64` regression), a single int64
  atomic SAD accumulator and a host-side `min(cur, next)` fold in `flush()`.
  Emits `VMAF_integer_feature_motion_v2_sad_score` and
  `VMAF_integer_feature_motion2_v2_score`.
- **`integer_motion_hip`**: raw-pixel ping-pong (`pix[2]`) and the shared
  diff-first SAD kernel of `motion_v2_hip` (`integer_motion_sad_hip.c`), so
  the SAD is the CPU `motion`'s:
  $\sum \lvert \mathrm{blur}(\mathrm{prev} - \mathrm{cur}) \rvert$, rounded
  after each
  pass (ADR-1377). Host-side `motion2` / `motion3` and the debug `motion`
  score go through the CPU's `motion_fps_weight` / `motion_max_val` clip.
  Emits `VMAF_integer_feature_motion2_score` and
  `VMAF_integer_feature_motion3_score`.

### Structure and colour

- **`float_ssim_hip`**: two-pass separable 11-tap Gaussian kernel. Pass 1
  (horizontal) writes five intermediate float buffers over (W-10)×H. Pass 2
  (vertical plus the SSIM combine) writes one `double` term per window over
  (W-10)×(H-10), stored at the window's raster position. The host adds the
  terms in raster order, as `iqa/ssim_tools.c` does, and rounds the mean to
  `float`. Above scale 1 a decimation kernel runs first and W×H is the
  decimated size. Emits `float_ssim`; see [float_ssim_hip](#float_ssim_hip).
- **`integer_ssim_hip`**: the CPU `ssim` extractor's algorithm, ported from
  the CUDA twin (`ssim_cuda.c`): a 9-tap integer Gaussian, int64 moments, the
  window truncated at the frame border, and the per-pixel SSIM term in
  double. Emits `ssim`; see [integer_ssim_hip](#integer_ssim_hip).
- **`integer_ms_ssim_hip`**: multi-scale SSIM over 5 pyramid levels, a 9-tap
  biorthogonal LPF decimation and a separable 11-tap Gaussian per scale.
  Emits `float_ms_ssim`, bit-identical to the CPU extractor; see
  [integer_ms_ssim_hip](#integer_ms_ssim_hip).
- **`ssimulacra2_hip`**: runs the whole frame on the device (ADR-1390, the
  HIP port of the SYCL chain of ADR-1363) with one upload of the raw Y/U/V
  planes and one 864-byte readback of per-scale sums. Bit-identical to the
  CPU extractor since ADR-1445 (the terms in double precision, the sums with
  the result of the CPU's loops), at 2.8 to 2.9 times the frame time; see
  [ssimulacra2](../../metrics/ssimulacra2.md#hip-device-resident-tiled-row-pass).
  Emits `ssimulacra2`.
- **`ciede_hip`**: the six Y/U/V planes on the device, per-pixel YUV→Lab
  conversion and CIEDE2000 ΔE in the CPU's arithmetic evaluated on pairs of
  `float` values (ADR-1448), one float per pixel read back, and the host's
  sum in the CPU's order and log10 transform. Within 1.4e-11 of the CPU
  extractor. Emits `ciede2000`; see [ciede_hip](#ciede_hip).

### ADM, VIF and CAMBI

- **`integer_adm_hip`** (registered as `adm_hip`): the full ADM DWT2, CSF, CM
  and decouple pipeline in five kernel files, mirroring `integer_adm_cuda.c`.
  Emits `integer_adm2` and the per-scale values; see [adm_hip](#adm_hip).
- **`integer_vif_hip`** (registered as `vif_hip`): the multi-scale VIF
  integer pyramid. It respects `vif_skip_scale0` (PR #1063) and
  `vif_enhn_gain_limit`, and emits `vif_scale0..3`, bit-identical to the CPU
  extractor (ADR-1435). See [vif_hip](#vif_hip).
- **`float_adm_hip`**: ADM float pipeline, the ninth kernel-template consumer
  (ADR-0468), bit-identical to the CPU `float_adm`. Emits `adm2`,
  `adm_scale0..3`, `aim` and `adm3`; see [float_adm_hip](#float_adm_hip).
- **`float_vif_hip`**: multi-scale VIF float pipeline, bit-identical to the
  CPU `float_vif`. Emits `float_vif_scale0..3`; see
  [float_vif_hip](#float_vif_hip).
- **`integer_cambi_hip`**: CAMBI banding detection, a full HIP port (PR #996,
  ADR-0345 Phase 3). Emits `cambi`.

## Floating-point policy

Every HIP kernel is compiled with one flag list, `hip_strict_fp_args` in
`core/src/meson.build`:

| Flag | What it does |
|---|---|
| `-ffp-contract=off` | hipcc contracts `a * b + c` into one fused multiply-add for device code by default. The CPU reference build does not, so a contracted kernel rounds differently from the extractor it mirrors. |
| `-fhip-fp32-correctly-rounded-divide-sqrt` | Correctly rounded fp32 `/` and `sqrtf()`. This is hipcc's default; it is passed explicitly so that a toolchain default cannot move a score. |

With the list, fp32 and fp64 `+ - * /` and `sqrt` in a kernel round as on the
CPU. Scores are still not bit-identical where a twin uses a transcendental
function, sums in another order, or computes in fp32 what the CPU computes in
fp64; the per-twin numbers are in
[ADR-1407](../../adr/1407-hip-strict-fp-every-kernel.md).

There is no per-kernel flag table: a kernel listed in `hip_kernel_sources`
gets the policy. Until ADR-1407 only `ssimulacra2_blur`, `integer_ssim_score`
and `speed_pipeline` turned contraction off, through `hip_cu_extra_flags`
([ADR-0594](../../adr/0594-hip-ssimulacra2-blur-fp-contract-off.md)).

Two tests guard it. `test_hip_strict_fp_policy.py` checks the build files and
needs no device. `test_hip_fp_arith_contract` compiles a probe kernel with the
same list and compares a million random `a * b + c`, `a / b` and `sqrtf(a)`
results from the device with correctly rounded host values:

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip test_hip_fp_arith_contract test_hip_strict_fp_policy
```

## Cost of exactness

Exact twins cost time. This table gives the frame time on the gfx1036 before
and after each twin took the CPU's arithmetic, from the pull request that
changed it (ms per frame).

| Twin | 1920x1080 before | 1920x1080 after | 3840x2160 before | 3840x2160 after |
|---|---|---|---|---|
| `vif_hip` | 44.4 | 42.7 | 188.2 | 163.4 |
| `integer_ssim_hip` | 28.2 | 30.0 | 94.3 | 98.1 |
| `float_psnr_hip` | 0.96 | 0.99 | 3.97 | 3.98 |
| `float_ssim_hip` | 1.72 | 2.02 | 4.86 | 5.18 |
| `float_ssim_hip`, `scale=1` | 17.7 | 23.4 | 82.3 | 109.6 |
| `float_vif_hip` | 20.7 | 26.0 | 86.0 | 147.1 |
| `ssimulacra2_hip` | 58.1 | 167.0 | 233.7 | 662.4 |
| `float_adm_hip` | 13.4 | 13.2 | 80.8 | 68.4 |
| `ciede_hip` | 18.6 | 49.6 | 75.6 | 210.1 |

`float_moment_hip` is unchanged. The open tuning rows are
`T-HIP-FLOAT-SSIM-EXACT-THROUGHPUT-2026-10-01`,
`T-HIP-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-02`,
`T-HIP-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02` and
`T-HIP-CIEDE-EXACT-THROUGHPUT-2026-10-02`.

### Why the two SpEED twins are exact

[ADR-1477](../../adr/1477-speed-upstream-double-math.md): the device runs
`speed.c` up to the per-block variances, and the host forms the entropies and
the score with `speed.c`'s own `log2()` calls, so no math library separates
the twin from the CPU. Before, the twin rounded `log2` on the device while
`speed.c` called glibc's `log2f`, and 6 of 534 `speed_chroma` values differed
by at most 1.4e-6.

Their rows in the overview were measured on the Netflix 576x324 pair at four
bit depths and in three chroma formats, both 1080p checkerboard pairs,
Sparks, noise at four bit depths, a bright 16-bit pair, two gradients and BBB
at 1920x1080 and 3840x2160.

### Why `ciede_hip` is bounded

`ciede_hip` runs the CPU's arithmetic as well and differs through a math
library; the gate bounds it by what that adds. glibc's `powf` is not
correctly rounded, which moves at most 74 of the 8.3 million pixels of a
3840x2160 frame by one `float` step. The twin also evaluates the CPU's
double-precision statements on pairs of `float` values, which moved 8 of 437
million pixels (see [ciede_hip](#ciede_hip)).

### float_ms_ssim sums

`float_ms_ssim` adds its windows on the host in the CPU's raster order. The
rounding of the per-scale means to `float` hid the earlier per-block sums on
every frame of the agreement table and not on one constructed noise frame; see
[the per-scale sums](#per-scale-sums).

## integer_ssim_hip

`integer_ssim_hip` publishes the same `ssim` feature as the CPU `ssim`
extractor (`integer_ssim.c`) and computes it the same way:

- a 9-tap Gaussian with integer weights `[2, 9, 28, 55, 68, 55, 28, 9, 2]`;
- int64 sums for the moments, which makes them exact;
- near the frame border the window is truncated to the taps inside the frame,
  as on the CPU;
- the per-pixel SSIM term in double, built with `-ffp-contract=off` so that it
  rounds like the CPU's.

### Frame sum and agreement

The frame sum is the CPU's as well, at every frame size
([ADR-1438](../../adr/1438-hip-ssim-cpu-frame-sum.md)): the device writes one
term per pixel, the host reads the plane back and adds it row by row, as
`calc_ssim()` does. A sum of doubles depends on its order, so the terms are
never added on the device; only the window weights, which are integers, are
reduced per block. The score is therefore the CPU's to the last bit, at every
bit depth, with and without `enable_db` / `clip_db`. Any frame size is
accepted.

Measured on a gfx1036 at `--precision max` against `--backend cpu`: 178 of 178
frames identical (the Netflix 576x324 pair at 8, 10, 12 and 16 bits and as
10-bit 4:2:2, both 1920x1080 checkerboard pairs, Sparks 480x270 at 10 bits, 48
frames of BBB 3840x2160, full-range noise at four depths, a bright 16-bit
1080p pair).

Until 2026-10-01 only frames of at most 4096 pixels were summed in the CPU's
order
([ADR-1400](../../adr/1400-hip-integer-ssim-raster-sum-small-frames.md));
larger frames were reduced per 16x8 block and 1 of those 178 frames matched,
the others up to 1.1e-11 away (on the 1080p checkerboard whose score is -0.53,
where terms of both signs cancel).

The read-back costs time and memory: 30.0 ms instead of 28.2 ms per 1920x1080
frame and 98.1 ms instead of 94.3 ms per 3840x2160 frame (medians of 21
interleaved pairs of runs under other load), and 8 bytes per pixel of device
and of pinned host memory (66 MB each at 3840x2160).

### Selection

- **Models.** When a model lists `ssim` and the HIP backend is active
  (`--backend hip`, or `vmaf_hip_import_state()` in the C API), the HIP twin
  computes it, with `enable_db` and `clip_db` if the model sets them.
- **CLI `--feature`.** `--backend hip --feature ssim` runs the HIP twin too
  ([ADR-1359](../../adr/1359-cli-feature-backend-twin.md)); the JSON output
  names the extractor that ran under `feature_backends`. Naming the twin
  always registers it:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --backend hip --feature integer_ssim_hip \
     --no_prediction --json --output ssim.json
```

The output has one `ssim` value per frame, as with `--feature ssim`.

## integer_ms_ssim_hip

`integer_ms_ssim_hip` publishes the `float_ms_ssim` feature of the CPU
extractor `float_ms_ssim` and returns the same numbers, bit for bit: the
score and, with `enable_lcs=true`, the 15 per-scale means
`float_ms_ssim_{l,c,s}_scale{0..4}`. `--backend hip` selects it for
`--feature float_ms_ssim` and for a model that lists the feature:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --backend hip --feature float_ms_ssim=enable_lcs=true \
     --no_prediction --json --output ms_ssim.json --precision max
```

It takes the CPU's options (`enable_lcs`, `enable_db`, `clip_db`,
`enable_chroma`) and the CPU's minimum frame size of 176x176. With
`enable_chroma=true` it scores the Cb and Cr planes through the same
pipeline and writes `float_ms_ssim_cb` and `float_ms_ssim_cr`, bit for bit
the CPU's; every scored plane must then be at least 176x176 (351x351 luma at
4:2:0). Until 2026-10-03 the option was accepted and ignored, and a run that
set it had no chroma scores ([MS-SSIM](../../metrics/ms-ssim.md)).

### Arithmetic

The kernels compute each sample the way the CPU does
([ADR-1403](../../adr/1403-cuda-strict-fp-every-kernel.md), which made the
CUDA twin exact first):

- the decimation between pyramid levels fuses every tap, as
  `ms_ssim_decimate.c` does;
- the Gaussian window sums are fp32 products added without rounding and
  rounded to fp32 once per pass, which is what the CPU's fp64 sum gives. The
  device carries the sum as an fp32 pair, because fp64 arithmetic is slow on
  it: an fp64 sum gave the same scores and 299 instead of 173 ms per
  3840x2160 frame;
- luminance and contrast divide an fp64 numerator by an fp32 denominator and
  structure is an fp32 quotient, as in `iqa/ssim_tools.c`;
- the device returns the l, c and s terms of every window and the host adds
  them in raster order into one `double` per sum, as `iqa/ssim_tools.c` does
  (see [the per-scale sums](#per-scale-sums));
- the host rounds each per-scale mean to fp32 and combines the scales with
  `fabs()` on all three terms, as `ms_ssim.c` does.

### Agreement and cost

Measured on a gfx1036 against `--backend cpu` at `--precision max` with
`enable_lcs=true`: the Netflix 576x324 pair (48 frames), the same pair at 10
bits (3), both 1080p checkerboard pairs (3 each) and BBB 3840x2160 (50) give
1712 values, all identical. Before, 6 were and no score: it was up to 3.0e-6
off and a per-scale mean up to 2.1e-5. `enable_db` / `clip_db` scores are
identical as well.

The exact sums cost time on this device: `float_ms_ssim` goes from 30.3 to
36.9 ms per 1920x1080 frame and from 158 to 169 ms per 3840x2160 frame
(medians of seven interleaved runs under other load; a second set of nine
gave 32.4 to 39.5 and 126 to 137).

### Per-scale sums

Until 2026-10-02 one step was the twin's own: the device added the l, c and s
terms of a scale per wave and per 16x8 block and the host added the blocks,
where `iqa/ssim_tools.c` adds every window into one `double` per sum in raster
order. Every add rounds, so the two sums differ in their last bits, and the
`float` rounding of the mean hides that except when a mean lies next to a
rounding boundary.

A search for such a mean (independent uniform noise at 176x176, the smallest
frame `float_ms_ssim` takes; a host build that forms both sums) found one in
5.28 million frames: on that pair the CPU returns
`float_ms_ssim_c_scale1` = 0.9854984283447266 (float bits `0x3f7c49a0`) and
the twin returned 0.9854983687400818 (`0x3f7c499f`), which moved
`float_ms_ssim` by 1.3e-9.

The pass-2 kernel now stores the three terms of every window and the host
adds each scale in raster order, the construction that made
`integer_ssim_hip` exact
([ADR-1438](../../adr/1438-hip-ssim-cpu-frame-sum.md)). The pair returns the
CPU's bits on all 16 outputs; `core/test/float_ms_ssim_order_frame.h` holds
its luma planes and `test_hip_ms_ssim_parity` compares every output with the
same build's CPU extractor.

### Per-scale sums: agreement and cost

Measured on a gfx1036 at `--precision max` against `--backend cpu`: on the
178 frames of the HIP sweep (fourteen fixtures, 480x270 to 3840x2160, 8 to 16
bits) `float_ms_ssim` is identical on 178 of 178 frames and all 2848 values
of `enable_lcs=true` are; the same with `enable_db` and `clip_db`. On 120
noise frames from 176x176 to 320x200 at 8, 10 and 16 bits all 2040 values are
identical.

The device now returns three `double` values per window instead of three per
block, and the host adds every window. Steady state, medians of 32
interleaved pairs of runs (two sets, with and without `enable_lcs`, which
does the same device and host work), while other work loaded the host:

| Frame | Before | After |
|---|---|---|
| 576x324 | 2.75 ms | 2.80 ms |
| 1920x1080 | 37.3 ms | 42.7 ms |
| 3840x2160 | 183.1 ms | 200.8 ms |

Single sets of seven or nine pairs ranged from +5 % to +47 % at 3840x2160
and from +9 % to +23 % at 1920x1080 (one set at 1080p alternated between 36
and 70 ms): the host loop reads 65 MB per 1920x1080 frame and 262 MB per
3840x2160 frame and competes with whatever else uses the host's memory. That
is also the memory the twin now holds twice, on the device and pinned on the
host: 24 bytes per window of every scale. The open tuning row is
`T-HIP-FLOAT-MS-SSIM-EXACT-THROUGHPUT-2026-10-02` in
[`docs/state.md`](../../state.md).

`test_hip_ms_ssim_arith` replays the kernels' arithmetic on the host against
the CPU extractor and needs no AMD device; `test_hip_ms_ssim_parity` compares
on one.

## adm_hip

`adm_hip` (`integer_adm_hip.c`) is the twin of the CPU `adm` extractor.
`--backend hip` picks it for `adm` and for a model's ADM features
([ADR-1525](../../adr/1525-adm-hip-aim-device-pass.md)); `--feature adm_hip`
names it directly. Its options mirror the CPU table entry for entry,
`adm_skip_aim` included.

### The default model's ADM

The default model `vmaf_v1.0.16_3d0h` requests
`VMAF_integer_feature_adm3_score` with `adm_csf_mode=2`,
`adm_dlm_weight=0.7`, `adm_enhn_gain_limit=1.0`, `adm_min_val=0.5` and
`adm_noise_weight=0.02`, under the key
`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.

`adm_hip` honours `adm_csf_mode` (all four CSF models) and
`adm_p_norm`, and its `VmafOption` table is an entry-for-entry mirror of the
CPU table, so every key it emits, `integer_adm3` and `integer_aim` included,
is identical to the CPU twin's for any options dict.

The AIM contrast measure runs on the device
([ADR-1525](../../adr/1525-adm-hip-aim-device-pass.md)): the kernels
`adm_cm_aim_line_kernel_4` (scale 0) and `i4_adm_cm_aim_line_kernel`
(scales 1 to 3) in `integer_adm/adm_cm.hip` are the HIP port of the CUDA
twin's ADR-0746 kernels. AIM swaps the roles of the DLM measure: the signal is
the CSF of the additive part a = t - r, the threshold the 3x3 neighbourhood of
|csf(r)| / 30 with the centre |csf(r)| / 15, recomputed from the wavelet bands
at every tap. Every rounding shift comes from the CPU's
`adm_cm_ctx_init()` / `i4_adm_cm_ctx_init()`, every row is folded once, and
the host concludes each scale with the CPU's `adm_cm_result()` /
`i4_adm_cm_result()` at noise weight 0 before it forms `aim` and blends
`adm3` with the CPU's own routines. `adm_skip_aim=true` launches no AIM
kernel and reports `aim` 0, as the CPU does.

Two CPU-parity corrections landed with the option work: `adm_min_val` no
longer clamps `adm2` (the CPU floors the adm3 expression only), and the
`numden_limit` precision floor scales with the full-frame area rather than the
scale-3 area.

### Agreement with the CPU

`--backend hip --feature adm_hip` gives the same `integer_adm2` and
`integer_adm_scale0..3` as `--backend cpu --feature adm`, to the last bit, and
with `debug=true` the same per-scale numerators and denominators
([ADR-1423](../../adr/1423-hip-adm-cpu-row-rounding.md)). Integer ADM is
integer arithmetic up to the conclusion of a scale, so the twin can have the
CPU's accumulators; it now takes its CSF weights, its border, its rounding
shifts and the conclusion from the CPU extractor's own routines, and rounds
the denominator once per row as the CPU does.

Measured on a gfx1036 at `--precision max`, 21 fixture pairs (the Netflix
576x324 pair at 8, 10, 12 and 16 bits and as 4:2:2, both 1080p checkerboard
pairs, a flat pair, sizes down to 18x22, synthetic noise, stripes, impulses
and blocks, and 200 frames of BBB 3840x2160):

| | Before | After |
|---|---|---|
| Pairs identical to the CPU | 19 of 21 | 21 of 21 (6192 values with `debug=true`) |
| Gradient against impulses, 576x324 | `integer_adm_scale3` 4.0e-7 off | identical |
| BBB 3840x2160 | `integer_adm_scale0` up to 1.4e-7 off | identical |
| A 962x13542 frame | `integer_adm_scale0` 0.860 (CPU 0.979) | 0.979 |
| First frame of a context after a smaller one in the same process | garbage, the run fails | identical |

The last two rows were defects on unusual input, not rounding. At 962x13542
the device computed a rounding shift with an fp32 logarithm that is off by one
for 81 region sizes, and the host concluded with the CPU's shift.

And each frame cleared the accumulators ahead of its upload, where on this
device the
clear is lost in the first context of a process that needs larger planes than
the contexts before it; the clear now follows the upload. The `vmaf` tool
creates one context per process and did not show the second defect; a program
that scores several clips through the library did.

The options keep their meaning: `adm_csf_mode` 1 to 3, the default model's
option set, `adm_enhn_gain_limit`, `adm_skip_scale0`, `adm_norm_view_dist` /
`adm_ref_display_height` and `adm_noise_weight` / `adm_p_norm` are identical
to the CPU on the same fixtures. A frame took 19.1 ms at 1920x1080 and about
80 ms at 3840x2160 on the gfx1036 before the AIM pass (77.4 and 80.4 in seven
interleaved runs whose samples overlap).

With the AIM pass (ADR-1525), measured at `--precision max` against
`--backend cpu` on the gfx1036: 4141 of 4141 values identical, `aim` and
`adm3` included (the Netflix pair at 8, 10, 12 and 16 bits with `debug=true`
and with the default model's options, both 1080p checkerboard pairs, 50 and
200 frames of BBB 3840x2160), and the default model's VMAF under
`--backend hip` equals `--backend cpu` on every frame of the Netflix pair, the
checkerboards and 50 frames of BBB 3840x2160. The cost on this 2-unit iGPU
(3840x2160, median of three, `adm` features only): 73 ms per frame with
`adm_skip_aim=true`, 218 ms with AIM, against 14.5 ms for the CPU extractor
with 16 threads (`T-HIP-ADM-AIM-INLINE-COST-2026-10-04`).

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip test_hip_adm_exact test_hip_adm_exact_contract
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu hip --features adm
```

## vif_hip

`--backend hip --feature vif_hip` gives the same `integer_vif_scale0..3` as
`--backend cpu --feature vif`, to the last bit, and with `debug=true` the same
frame ratio and per-scale numerator and denominator sums
([ADR-1435](../../adr/1435-hip-vif-cpu-log2-table.md)).

The fixed-point VIF
statistic is integer arithmetic up to its last step and takes every per-pixel
logarithm from a table of 32768 entries that the CPU extractor fills with the
host math library. The twin used to evaluate `log2f()` on the device instead,
which is one ulp from glibc's for about half of the arguments and rounded
ties the other way; 77 entries came out one lower. It now uploads the CPU's
table at `init()` (64 KB) and the kernels look every logarithm up.

Measured on a gfx1036 at `--precision max`, frames whose score equals the
CPU's on scale 0 / 1 / 2 / 3:

| Fixture | Frames | Before | Max abs diff before | After |
|---|---|---|---|---|
| Netflix 576x324, 8 bit | 48 | 4 / 0 / 1 / 1 | 5.4e-7 | 48 / 48 / 48 / 48 |
| Checkerboard 1 px, 1920x1080 | 3 | 2 / 3 / 2 / 3 | 3.0e-8 | 3 / 3 / 3 / 3 |
| Checkerboard 10 px, 1920x1080 | 3 | 3 / 3 / 3 / 3 | 0 | 3 / 3 / 3 / 3 |
| Netflix 576x324, 10 bit | 3 | 1 / 0 / 0 / 0 | 3.6e-7 | 3 / 3 / 3 / 3 |
| Sparks 480x270, 10 bit | 5 | 0 / 0 / 3 / 1 | 3.6e-7 | 5 / 5 / 5 / 5 |
| BBB 3840x2160 | 48 | 5 / 3 / 5 / 3 | 3.0e-7 | 48 / 48 / 48 / 48 |

49 of 440 scores before, 440 of 440 after. After the change the fifteen
outputs of `debug=true` are identical too on those fixtures and on the
Netflix pair at 12 and 16 bits and as 10-bit 4:2:2 (2460 values), and the four
scores with `vif_enhn_gain_limit=1.0` and with `vif_skip_scale0=true` on the
same eight small fixtures (464 values each).

A frame takes no longer than before. Steady state inside one process, 21
interleaved pairs of runs while other lanes loaded the host (load average 10
to 95): 44.4 ms before and 42.7 ms after at 1920x1080, 188.2 and 163.4 ms at
3840x2160 (medians; the samples range from 32.3 to 52.8 and 32.4 to 49.1 ms,
and from 137.5 to 212.4 and 134.3 to 194.6 ms). A lookup replaces each
`log2f()`, and a pixel in the low-variance branch no longer computes the fp64
gain it does not use.

Stored `vif_hip` scores change by up to 5.4e-7; re-run them if you compare
against the CPU at full precision.

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip \
    test_hip_vif_parity test_hip_vif_parity_large test_hip_vif_log2_table_contract
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu hip --features vif
```

## float_moment_hip

`--backend hip --feature float_moment_hip` gives the same four
`float_moment_*` values as `--backend cpu --feature float_moment`, to the
last bit ([ADR-1447](../../adr/1447-hip-float-moment-cpu-float-squares.md)).
The CPU forms each sample's square in `float` before adding it. Up to 12 bits
per sample that is the exact square; at 16 bits it is the square rounded to
24 bits. The twin added exact squares, so its second moments
(`float_moment_ref2nd`, `float_moment_dis2nd`) were off at 16 bits. It now
adds the same rounded square as the CPU.

### Up to 2^53 units

Measured on a gfx1036 at `--precision max`, frames whose second moments equal
the CPU's:

| Fixture | Frames | Before | Max abs diff before | After |
|---|---|---|---|---|
| Typical content at 8 and 10 bit (Netflix 576x324, 1080p checkerboards, Sparks, BBB 3840x2160) | 110 | 110 | 0 | 110 |
| Netflix 576x324 at 12 and 16 bit and as 10-bit 4:2:2; noise at 8, 10, 12 bit | 63 | 63 | 0 | 63 |
| Full-range noise 576x324, 16 bit | 3 | 0 | 2.8e-5 | 3 |
| Bright 16 bit, 1920x1080 | 2 | 0 | 1.0e-4 | 2 |
| BBB 1920x1080 as 16 bit | 40 | 0 | 7.5e-5 | 40 |
| BBB 3840x2160 as 16 bit | 32 | 0 | 3.9e-5 | 32 |

The first moments were identical before and are now. The repository's 16-bit
Netflix fixture is 8-bit content shifted left, which is why it never showed
the difference. If you stored 16-bit `float_moment_hip` second moments,
re-run them.

Before ADR-1497 one range was not bit-identical. The CPU adds the squares
into a `double`, which holds the sum exactly up to $2^{53}$ in units of
$2^{-16}$. A
frame of up to 2 097 152 pixels (1920x1080 has 2 073 600) cannot reach that,
and neither can any frame at 8, 10 or 12 bits.

A larger 16-bit frame whose second moment times its pixel count reaches $2^{37}$
does reach it. From there the CPU's sum rounds as it goes, and the twin,
which added exactly, could differ from it by at most
$\dfrac{\mathrm{pixels} - 2^{21} + 1}{\mathrm{pixels}} \cdot 2^{e - 69} + 2^{-37}$
(`e` is 53 or 54 up to
3840x2160; 2.3e-5 at 3840x2160 with every sample near the peak).

Measured then: 2.7e-7 on a 2560x1440 frame with a tenth of its samples below
4096, 1.2e-7 on full-range 3840x2160 noise, and 0 on the 17 frames of the
16-bit BBB 3840x2160 fixture that are in that range. Since 2026-10-03 that
range is bit-identical too (see [Past $2^{53}$ units](#past-253-units)).

The change costs nothing measurable: 1.94 and 2.02 ms per 16-bit 1920x1080
frame before and after, 11.1 and 10.6 ms per 16-bit 3840x2160 frame (medians
of 11 interleaved pairs; the samples overlap).

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip \
    test_hip_float_moment_parity test_hip_float_moment_parity_large \
    test_hip_float_moment_exact_contract
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu hip --features float_moment
```

### Past 2^53 units

`float_moment_hip` now returns the CPU extractor's four moments bit for bit
on every frame
([ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md)).
The CPU adds the float squares into one `double` in raster order. Below $2^{53}$
units of $2^{-16}$ that sum is exact and equal to the twin's integer sum, which
covers every frame of up to 2 097 152 pixels and every 8-, 10- and 12-bit
frame. On a larger 16-bit frame whose sum passes $2^{53}$ the CPU rounds as it
adds, and the twin used to round the exact sum once.

It now forms the CPU's
rounded sum: on such a frame four more kernels add each row exactly while the
sum is at or below $2^{53}$, then from integer increments of the sum's last
place, composed in pixel order and checked against the exact running sum,
and term by term where a row crosses into the next binade
(`core/src/feature/float_moment_sum.h`).

Frames that cannot pass $2^{53}$ run no
new work.

Measured on a gfx1036 at `--precision max` against `--backend cpu`, frames
whose four outputs are identical and the largest difference:

| Fixture | Before | Now |
|---|---|---|
| Full-range 16-bit noise 3840x2160, nine tenths near the peak, 16 frames | 0 of 16, 2.7e-7 | 16 of 16 |
| The same at 7680x4320, 4 frames | 0 of 4, 5.1e-7 | 4 of 4 |
| BBB 3840x2160 widened to 16 bits (shifted left by 8, times 257, full range with a dithered low byte), 32 frames each, 17 of them past $2^{53}$ | 32 of 32 | 32 of 32 |

BBB was identical before as well: its widened samples have no bits below the
sum's last place until $2^{55}$, which a 3840x2160 frame cannot reach. The parity
gate's `float_moment` cell reads 0 at tolerance 0 on the 16-bit 3840x2160
noise (it failed there before).

Time per 16-bit 3840x2160 frame through libvmaf, pictures preloaded, medians
of 5 interleaved runs at a load average of 4 to 5, before and after:

| Input | Before | After |
|---|---|---|
| Noise, every frame past $2^{53}$ | 18.40 ms | 25.62 ms |
| BBB full range, 17 of 32 frames past $2^{53}$ | 18.37 ms | 21.28 ms |

The added time is the row-increment kernel on the two compute units of the
gfx1036: `T-GPU-FLOAT-MOMENT-EXACT-SUM-COST-2026-10-03` (RC8).

```shell
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference ref_16bit_3840x2160.yuv --distorted dis_16bit_3840x2160.yuv \
    --width 3840 --height 2160 --bitdepth 16 --backends cpu hip --features float_moment
```

## ciede_hip

`ciede.c` computes in double precision and stores in single, and adds every
pixel's colour difference into one `double` in raster order. `ciede_hip`
computed in single precision with another form of the formula and added per
wave and per 16x16 block: it matched the CPU on no frame and was up to 1.1e-5
from it.

Since [ADR-1448](../../adr/1448-hip-ciede-cpu-arithmetic.md) the
kernel runs the CPU's statements with every `double` as a pair of `float`
values and every math function as a routine on such pairs. This is the
arithmetic of the SYCL twin, from the same header
(`core/src/feature/ciede_ff_math.h`). The kernel stores one `float` per
pixel, and the host adds them in the CPU's order.

The gfx1036 has `double`, and a first version ran the CUDA twin's
double-precision statements. Its math functions made a 1920x1080 frame take
318 ms instead of 18 ms, so that version was not merged.

### Agreement

Measured on a gfx1036 (ROCm 7.2.4, glibc 2.44) at `--precision max` against
`--backend cpu`:

| Fixture | Frames | Identical before | Max abs diff before | Identical after | Max abs diff after |
|---|---|---|---|---|---|
| Netflix 576x324, 8 bit | 48 | 0 | 1.1e-5 | 47 | 6.9e-13 |
| Checkerboard 1 px and 10 px, 1920x1080 | 6 | 0 | 8.6e-7 | 6 | 0 |
| Netflix 576x324, 10, 12 and 16 bit | 9 | 0 | 9.4e-6 | 9 | 0 |
| Netflix 576x324, 10-bit 4:2:2 | 48 | 0 | 1.1e-5 | 46 | 2.9e-12 |
| Sparks 480x270, 10 bit | 5 | 0 | 1.1e-6 | 4 | 2.4e-12 |
| BBB 3840x2160 | 48 | 0 | 1.4e-6 | 0 | 1.4e-11 |
| Full-range noise 576x324 at 8, 10, 12, 16 bit | 12 | 0 | 2.3e-7 | 3 | 4.6e-12 |
| Bright 16 bit, 1920x1080 | 2 | 0 | 1.6e-6 | 0 | 1.8e-12 |

### Why it is not bit-identical

The twin is not bit-identical, for two measured reasons. Of 437 million
pixels compared one by one with a host replay of the CPU's statements, 2 214
differ. 2 206 of them differ by one `float` step because glibc's `powf` is not
correctly rounded where the kernel's value is. The other 8, all in the first
twelve BBB frames, differ by one to nine steps because a pair holds 48 bits
where a `double` holds 53. The parity gate bounds the cell at `1e-9`
([cross-backend gate](../../development/cross-backend-gate.md)).

### Cost

The pair arithmetic costs time (ms per frame, steady state, medians of three
interleaved pairs of runs):

| Frame | Before | After | |
|---|---|---|---|
| 1920x1080 | 18.6 | 49.6 | 2.7x |
| 3840x2160 | 75.6 | 210.1 | 2.8x |

At 1920x1080 the two L\*a\*b\* conversions take 21.9 ms and the colour
difference 25.5 ms; uploading the planes, launching the kernel, reading one
`float` per pixel back and the host's sum take 2.2 ms. At 3840x2160 the three
are 86.8, 113.5 and 9.8 ms.

The CPU extractor takes 136 ms per 3840x2160 frame on sixteen threads, so on
this integrated GPU the twin is slower than the CPU
at that size (`T-HIP-CIEDE-EXACT-THROUGHPUT-2026-10-02`). The twin also keeps
one `float` per pixel on the device and on the host, 33 MB each at 3840x2160.

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip test_hip_ciede_parity test_hip_ciede_math
python3 scripts/dev/speed_gpu_parity.py --backend hip --feature ciede \
    --max-abs-diff 1e-9 --vmaf "$PWD/build-hip/tools/vmaf"
```

## float_adm_hip

`float_adm_hip` computed the float ADM scores with eight small differences
from the CPU extractor: another association in the angle test, rows added in
partial sums per wave and in `double` on the host, its own copy of the CSF
weights, single-precision constants and gain limit, another order in the
masking threshold, another floor for the frame sums.

It matched the CPU on
224 of 1246 measured values and was up to 1.3e-5 from it. Since
[ADR-1458](../../adr/1458-hip-float-adm-cpu-arithmetic.md) it runs the
arithmetic of the CUDA twin, from the same header
(`core/src/feature/float_adm_gpu_common.h`), and takes the weights, the
reduced region and the pooling from the CPU's own routines. Every score is
the CPU's, bit for bit.

Measured on a gfx1036 (ROCm 7.2.4) at `--precision max` against
`--backend cpu`, values identical over the seven scores of every frame (110
frames of typical content, 68 that stress the arithmetic):

| Run | Before | After |
|---|---|---|
| Default options | 224 of 1246 (largest difference 1.3e-5) | 1246 of 1246 |
| `debug=true` (18 outputs per frame) | not measured | 3204 of 3204 |
| `adm_enhn_gain_limit=1.2` | not measured | 1246 of 1246 |
| `adm_bypass_cm=1` | not measured | 1246 of 1246 |
| `adm_skip_aim_scale=1` | not available | 1246 of 1246 |
| `adm_norm_view_dist=1.5` | not measured | 1246 of 1246 |
| `adm_p_norm=1` | not measured | 1246 of 1246 |
| `adm_p_norm=2` | not measured | 911 of 1246 (largest difference 1.5e-7) |

What changed for a user:

- Scores from before the change differ from new ones by up to 1.3e-5.
- The twin takes `adm_skip_aim_scale` (alias `sasc`), which it rejected
  before, with the CPU's meaning: that scale is left out of the `aim` score.
- A frame smaller than 17x17 is refused at start with the CPU's error. The
  twin scored such frames before, from bands of one sample.
- `adm_p_norm` other than 1 or 3 is not identical: both sides raise every
  term with `powf`, the CPU with the C library's and the twin with the
  device's.
- The twin uses 48 MB more device memory at 3840x2160.

It costs no time (ms per frame, steady state, medians of nine interleaved
pairs of runs on a loaded host): 13.4 before and 13.2 after at 1920x1080,
80.8 and 68.4 at 3840x2160.

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip test_hip_float_adm_parity test_hip_float_adm_math
python3 scripts/dev/speed_gpu_parity.py --backend hip --feature float_adm \
    --vmaf "$PWD/build-hip/tools/vmaf"
```

## float_vif_hip

`--backend hip --feature float_vif_hip` gives the same `vif_scale0..3` as
`--backend cpu --feature float_vif`, to the last bit, and with `debug=true`
the same frame ratio and per-scale numerator and denominator sums
([ADR-1444](../../adr/1444-hip-float-vif-cpu-arithmetic.md)). The twin runs
the arithmetic of the CUDA twin from one shared header
(`core/src/feature/float_vif_gpu_common.h`): the Gaussian taps the CPU computes
with `vif_get_filter()`, the CPU's polynomial `log2`, the noise variance as a
`double`, and one `float` sum per row and then over the rows.

Before, the kernel held a table of taps the CPU no longer uses, called the
device `log2f()`, took the noise variance as a `float` and added per wave and
per 16x16 block.

### Agreement

Measured on a gfx1036 at `--precision max`, frames whose score equals the CPU's
on scale 0 / 1 / 2 / 3:

| Fixture | Frames | Before | Max abs diff before | After |
|---|---|---|---|---|
| Netflix 576x324, 8 bit | 48 | 0 / 0 / 0 / 0 | 3.8e-5 | 48 / 48 / 48 / 48 |
| Checkerboard 1 px, 1920x1080 | 3 | 0 / 0 / 0 / 0 | 1.05e-6 | 3 / 3 / 3 / 3 |
| Checkerboard 10 px, 1920x1080 | 3 | 1 / 3 / 3 / 3 | 1.1e-12 | 3 / 3 / 3 / 3 |
| Netflix 576x324, 10 bit | 3 | 0 / 0 / 0 / 0 | 1.07e-5 | 3 / 3 / 3 / 3 |
| Sparks 480x270, 10 bit | 5 | 0 / 0 / 0 / 0 | 3.4e-6 | 5 / 5 / 5 / 5 |
| BBB 3840x2160 | 48 | 0 / 0 / 0 / 0 | 7.0e-6 | 48 / 48 / 48 / 48 |
| Netflix 576x324, 12 and 16 bit | 3 each | 0 / 0 / 0 / 0 | 1.07e-5 | all |
| Netflix 576x324, 10-bit 4:2:2 | 48 | 0 / 0 / 0 / 0 | 3.8e-5 | 48 / 48 / 48 / 48 |
| Full-range noise 576x324 at 8, 10, 12, 16 bit | 3 each | 0 / 0 / 0 / 0 | 1.0e-8 | all |
| Bright 16 bit, 1920x1080 | 2 | 0 / 0 / 0 / 0 | 1.06e-4 | 2 / 2 / 2 / 2 |

10 of 712 scores before, 712 of 712 after. The 1.06e-4 of the bright 16-bit
pair was above the 5e-5 the parity gate allowed the twin. With `debug=true`,
`vif_enhn_gain_limit=1.0` with `vif_sigma_nsq=1.5`, `vif_sigma_nsq=4.7`,
`vif_skip_scale0` and the per-scale floors (`vif_scale1_min_val`,
`vif_scale3_min_val`, new on this twin) the outputs are identical too on 62
frames of six of those fixtures (1922 values).

The old kernel also read in front of its buffer on a plane smaller than a
16x16 tile, and the gfx1036 faults on that read: `float_vif_hip` on a 64x64,
56x56 or 40x40 frame ended with `Memory access fault by GPU node-1` on three
of three runs each. Frames from 16x16 now run and return the CPU's bits
(16x16, 17x33, 71x20 checked).

### Cost

A frame takes longer. Steady state inside one process, 11 interleaved pairs
of runs, host load average 3 to 13:

| Frame | Before | After | |
|---|---|---|---|
| 1920x1080 | 20.7 ms | 26.0 ms | +26 % |
| 3840x2160 | 86.0 ms | 147.1 ms | +71 % |

Where the increase goes, measured by taking one property out of the new twin
at a time: evaluating the two quotients in `double` costs 4.4 ms at 1920x1080
and 8.3 ms at 3840x2160; the plane of per-pixel terms that the row sums read
(two floats per pixel, 66 MB at 3840x2160) costs 1.1 ms and 37.8 ms; adding
each row in one thread costs 0.6 ms at 1920x1080 and nothing measurable at
3840x2160.

On this integrated GPU the twin is slower than the CPU extractor
(46 ms per 3840x2160 frame on 16 threads).

Tuning is tracked as
`T-HIP-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-02`; the scores must stay
bit-identical.

Stored `float_vif_hip` scores change by up to 3.8e-5 on typical content;
re-run them if you compare against the CPU.

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip \
    test_hip_float_vif_parity test_hip_float_vif_parity_large \
    test_hip_float_vif_exact_contract test_float_vif_device_math
python3 scripts/dev/speed_gpu_parity.py --backend hip \
    --vmaf "$PWD/build-hip/tools/vmaf" --feature float_vif
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu hip --features float_vif
```

## Motion twins, tiny frames and CPU options

The motion twins, the tile loads and the option tables of four twins follow
the CPU extractors. Device tests check them on any AMD device; the commands
are at the end of [float_motion_hip options](#float_motion_hip-options). The
first device run is recorded in the
[history](history.md#measured-on-a-gfx1036-after-the-rc3-parity-change).

### motion_hip

`motion_hip` matches the CPU `motion` exactly
([ADR-1377](../../adr/1377-hip-motion-diff-first.md)). The CPU differences the
two frames, blurs the difference, and rounds after the vertical and after the
horizontal pass. `motion_hip` used to blur each frame and difference the
blurred frames, which rounds differently (1.26e-5 on the Netflix pair).

It now runs the kernel `motion_v2_hip` already used
(`integer_motion_v2/motion_v2_score.hip`, launched only by
`integer_motion_sad_hip.c`), and its debug `motion` score carries
`motion_fps_weight` and `motion_max_val` like the CPU's.

Both motion twins
read the frame's luma from the shared planes and keep the previous frame in a
device plane of their own (see [Picture uploads](uploads.md#picture-uploads)).

### Small frames

Small frames stay inside the device buffers
([ADR-1381](../../adr/1381-hip-integer-tiny-frame-guards.md)). A tiled kernel
loads a whole tile for every thread, padding threads included, and reflects an
out-of-plane index once; on a plane smaller than the tile that reflection can
still land outside it. The motion and float-motion tile loads and the integer
ADM scale-0 vertical DWT now clamp the reflected index into the plane
(`hip_tile_index.h`, `integer_adm/adm_dwt2_rows.h`). The clamp is the identity
for every sample an output reads, so no score changes.

`test_hip_adm_dwt2_rows` replays every thread of the launch for every plane
height up to 8192 on the host.

`vif_hip` now needs 16x16 frames, as `vif_sycl`
does: its filters reflect once per scale and need 16 pixels at scale 3. With a
model, smaller frames run on the CPU `vif`; `--feature vif_hip` below 16x16
fails at init.

### CPU options of four twins

Four twins take the CPU options
([ADR-1382](../../adr/1382-hip-twin-cpu-option-parity.md)):

| Twin | Options added | How |
|---|---|---|
| `psnr_hip` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | Host, from the device SSE, through the CPU's `psnr_score.h`; `apsnr_*` in `flush()` |
| `integer_ssim_hip` | `enable_db`, `clip_db` | Host, `vmaf_ssim_max_db()` and the shared SSIM emitter |
| `float_ssim_hip` | `enable_lcs`, `enable_db`, `clip_db` | `enable_lcs` selects a pass-2 kernel that also returns the L, C and S of every window |
| `float_motion_hip` | `motion_max_val` (`mmxv`) | Host; every emitted score, the debug one included, goes through the CPU's `motion_clip()` |

With `enable_db`, identical frames report what the CPU reports. For
`integer_ssim_hip` that is `+inf` (or the `clip_db` ceiling) on frames above
4096 pixels. Smaller frames report the CPU's own value, which is usually `+inf`
and sometimes a finite value one or two ulps below a perfect score: 156.54 dB
on an identical 1x1 frame of zeros, 159.55 dB on a flat 3x3 frame of 51
([ADR-1400](../../adr/1400-hip-integer-ssim-raster-sum-small-frames.md)).

`float_ssim_hip` computes each pixel's term as the CPU does, `l * c * s` from
the CPU's own luminance, contrast and structure types, adds the windows in
the CPU's raster order and rounds the frame mean to fp32 like the CPU. On
some identical frames the CPU's fp32 arithmetic
leaves 1 - $2^{-24}$, which is 72.247 dB, and the twin reports the same instead
of a forced `+inf`.

### Other CPU behaviours

Three more CPU behaviours are matched. `motion_v2_hip` stores its SAD weighted
by
`motion_fps_weight` and capped at `motion_max_val`, as the CPU does, and
emits `motion2_v2` / `motion3_v2` for a one-frame run. `psnr_hip` sees every
frame under `--subsample`, so the `apsnr_*` totals cover the clip.
`motion_hip` defaults `debug` to false and writes
`VMAF_integer_feature_motion_sad_score` every frame, like the CPU `motion`.

With `motion_force_zero=true`, `motion_hip` and `float_motion_hip` write the
CPU's zero scores; before ADR-1382 both crashed on the first frame.

### motion_five_frame_window

`motion_hip` and `motion_v2_hip` compute `motion_five_frame_window`
([ADR-1491](../../adr/1491-gpu-motion-five-frame-window.md)). With
the option each twin keeps two earlier luma planes instead of one, so frame
`n`'s SAD reads frame `n-2`, and derives `motion2` / `motion3` with the CPU's
window function at the end of the run ([Motion, five-frame
window](../../metrics/motion.md#five-frame-window)).

On a gfx1036 every output equals the CPU's: `test_hip_motion_five_frame_window`
(six option sets, 11, 1, 2 and 3 frames, 8 and 10 bits), and 1352 of 1352
motion values on the Netflix 576x324 pair at 8 and 10 bits, a 1080p
checkerboard pair and 50 frames of BBB 3840x2160, an `_hfr` model among the
four option sets.

### float_motion_hip options

`float_motion_hip` emits `motion3` and takes the whole CPU `float_motion`
option table
([ADR-1404](../../adr/1404-hip-float-motion-motion3-and-options.md)):

| Option | Where it runs |
|---|---|
| `motion_blend_factor` (`mbf`), `motion_blend_offset` (`mbo`) | Host: `motion3` is the CPU's blend of the fps-weighted score, then the `motion_max_val` cap |
| `motion_filter_size` (`mfs`) | Kernel argument: `3` selects the 3-tap filter, `1` no blur, anything else the 5-tap filter |
| `motion_add_scale1` (`mdc`) | A second kernel scales both blurred frames to half size with the CPU's bilinear scaler and stores their differences; the row kernel adds them |
| `motion_add_uv` (`mau`) | The same kernels run on the U and V planes and the three scores add up; 4:0:0 input is refused |

A frame is still one upload call, one read-back and one wait, whatever the
options.

### Exact sums

`float_motion_hip` returns the CPU extractor's scores bit for bit, with every
option ([ADR-1419](../../adr/1419-hip-float-motion-cpu-float-sum.md)). The CPU
adds the absolute differences of a row into one `float`, the row sums into a
second one, and divides in `float`, and those running sums round at every
step, so the score depends on the order of the additions.

The twin used to add
16x16 blocks and was 3e-6 (576x324) to 1.4e-4 (1080p checkerboards) from the
CPU. It now stores every absolute difference and adds each row left to right on
the device, one thread per row, and the rows on the host.

The differences are stored transposed, 64 rows to a group with a column's
samples adjacent,
because the threads of a wave walk different rows: read from the blurred
planes, the row sums took 145 ms per 3840x2160 frame on the gfx1036.

Measured on a gfx1036 against `--backend cpu` at `--precision max`, `motion`,
`motion2` and `motion3` on the Netflix 576x324 pair at 8 bits (48 frames) and
10 bits (3), both 1080p checkerboard pairs (3 each) and BBB 3840x2160 (20
frames), 231 values per option set:

| Options | Identical before | Max abs diff before | Identical after |
|---|---|---|---|
| none | 10 | 1.36e-4 | 231 |
| `motion_add_scale1=true` | 10 | 2.21e-4 | 231 |
| `motion_add_uv=true` | 10 | 1.36e-4 | 231 |
| `motion_add_scale1=true:motion_add_uv=true` | 10 | 2.21e-4 | 231 |
| `motion_filter_size=3` | 10 | 1.58e-4 | 231 |
| `motion_filter_size=1` | 10 | 2.46e-5 | 231 |
| `motion_fps_weight=1.5:motion_blend_factor=0.5:motion_blend_offset=2:motion_max_val=4` | 177 | 2.62e-6 | 231 |

The 10 values that matched before are the zero scores of first frames, and
the 177 of the last row are mostly scores at the `motion_max_val` cap.

Time per frame on the gfx1036, medians of seven interleaved runs of both
builds with other jobs loading the host:

| Run | 1920x1080 before | 1920x1080 after | 3840x2160 before | 3840x2160 after |
|---|---|---|---|---|
| default options | 2.50 | 2.78 | 18.09 | 19.76 |
| `motion_add_scale1` | 3.03 | 3.74 | 21.02 | 24.68 |
| `motion_add_uv` | | | 19.05 | 20.52 |

`--model version=vmaf_float_v0.6.1`, of which `float_motion_hip` is one
extractor, reads 49.98 and 50.11 ms per 1920x1080 frame and 192.66 and 199.17
per 3840x2160 frame before and after (medians of five interleaved runs), which
is inside the spread of the runs.

### Verifying on an AMD host

To confirm on an AMD host, build with HIP in the `vmaf-dev-mcp` container and
run the device tests, which skip (exit 77) without a device:

```bash
meson setup build-hip core -Denable_hip=true -Denable_hipcc=true \
    -Dhip_gfx_targets=gfx1036
ninja -C build-hip
python3 scripts/ci/run_meson_test.py -- -C build-hip \
    test_hip_motion_tiny_frames test_hip_twin_option_parity \
    test_hip_vif_min_dim test_hip_adm_tiny_frames test_hip_upload_race \
    test_hip_float_motion_parity test_hip_float_motion_rows
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 --backends cpu hip \
    --features float_ssim float_ssim_lcs psnr motion_v2 vif float_motion
```

Replace `gfx1036` with your device's target (`rocm_agent_enumerator` prints
it). The per-row commands, with the expected numbers and a timing run, are in
[`docs/state.md`](../../state.md) under `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29`,
`T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29`,
`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29` and
`T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26`.

## float_ssim_hip

`float_ssim_hip` returns the CPU's score bit for bit at every frame size and
with every option. The sections below give the decimation, the exact window
sums and the frame sum.

### Decimation at 1080p and 4K

The CPU `float_ssim` decimates both planes before SSIM by
$\max(1, \operatorname{round}(\min(w, h) / 256))$: 1 below 384 px, 4 at
1920x1080, 8 at
3840x2160; the `scale` option forces the factor. `float_ssim_hip` used to
implement scale 1 only, so at 1080p and 4K `--backend hip --feature float_ssim`
and models computed the feature on the CPU and printed
`float_ssim_hip cannot run 3840x2160 8-bit pictures with these options`.
Since [ADR-1405](../../adr/1405-hip-float-ssim-device-decimation.md) the twin
decimates on the device, with the CPU's reduced planes bit for bit, and runs
every size:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 3840 --height 2160 --pixel_format 420 --bitdepth 8 \
     --backend hip --feature float_ssim \
     --no_prediction --json --output ssim.json
# ssim.json: "feature_backends": [{"extractor": "float_ssim_hip", "backend": "hip"}]
```

The twin falls back to the CPU extractor only when the decimated plane is
smaller than the 11x11 SSIM window (for example 100x100 at `scale=10`) or the
scale is above 128; `--feature float_ssim_hip` fails at init in those cases.

Measured on a gfx1036 against `--backend cpu` at `--precision max` when
ADR-1405 landed:

| Clip | Scale | Max abs diff | Twin | CPU, 16 threads | Before (CPU fallback) |
|---|---|---|---|---|---|
| 3840x2160, 50 frames | 8 (auto) | 1.79e-6 | 5.5 ms/frame | 11.9 ms/frame | 19.7 ms/frame |
| 1920x1080, 24 frames | 4 (auto) | 4.23e-6 | 2.0 ms/frame | 2.7 ms/frame | 6.6 ms/frame |
| Netflix 576x324, 48 frames | 1 (auto) | 1.79e-7 | unchanged | — | — |
| Netflix 576x324 | 2, 3, 5, 10 | at most 1.79e-7 | — | — | — |

### Window sums

The differences above are gone
([ADR-1441](../../adr/1441-hip-float-ssim-cpu-window-sums.md)). The CPU adds
the eleven products of a Gaussian window in `double` and rounds once per
pass; the twin added them in single precision. It now forms both window
passes and the luminance, contrast and structure terms through the same
arithmetic as `integer_ms_ssim_hip`, which carries the `double` sum as an
exact pair of floats.

Measured on a gfx1036 at `--precision max`: 178 of 178 frames identical to
`--backend cpu` (27 before; the Netflix 576x324 pair at 8, 10, 12 and 16 bits
and as 10-bit 4:2:2, both 1920x1080 checkerboard pairs, Sparks 480x270 at 10
bits, 48 frames of BBB 3840x2160, full-range noise at four depths, a bright
16-bit 1080p pair), and all four outputs of `enable_lcs=true` on the same
frames (712 values). `scale=1`, `scale=3`, `enable_db` and `clip_db` are
identical too.

The exact sums cost time. Steady state, medians of 11 interleaved pairs of
runs:

| Run | Before | After | Change |
|---|---|---|---|
| 1920x1080, default scale (4) | 1.72 ms | 2.02 ms | +17 % |
| 3840x2160, default scale (8) | 4.86 ms | 5.18 ms | +7 % |
| 1920x1080, `enable_lcs=true` | 1.98 ms | 2.31 ms | +17 % |
| 1920x1080, `scale=1` | 17.7 ms | 23.4 ms | +32 % |
| 3840x2160, `scale=1` | 82.3 ms | 109.6 ms | +33 % |

Stored `float_ssim_hip` scores change by up to 4.8e-7.

### Frame sum

One step was still the twin's own. `iqa/ssim_tools.c` adds the term of every
window into one `double` in raster order and returns
`(float)(sum / windows)`. The twin added 16x16 blocks on the device and the
blocks on the host. The two `double` sums differ in their last bits, and the
`float` rounding of the mean hides that except when the mean lies next to a
rounding boundary.

A search over noise frames found such a frame: one 64x64
8-bit pair on which the CPU returns -4.222829943500983e-07 (float bits
`0xb4e2b622`) and the twin returned -4.222829659283889e-07 (`0xb4e2b621`).

The pass-2 kernel now stores the `double` term of every window and the host
adds them in raster order, which is how `integer_ssim_hip` became exact
([ADR-1438](../../adr/1438-hip-ssim-cpu-frame-sum.md)). With
`enable_lcs=true` the same holds for the `l`, `c` and `s` sums. The twin
returns `0xb4e2b622` on that pair; `core/test/float_ssim_order_frame.h` holds
the pair and `test_hip_float_ssim_parity` compares the bits with the constant
and with the same build's CPU extractor.

### Frame sum: agreement and cost

Measured on a gfx1036 at `--precision max` on the 178 frames listed above:
`float_ssim` 178 of 178 identical to `--backend cpu`, `enable_lcs=true` 712 of
712, `scale=1` 178 of 178, `scale=1` with `enable_lcs=true` 712 of 712,
`scale=3` 178 of 178, `enable_db` with `clip_db` 178 of 178. On 160 small
noise frames (11x11 to 100x60 at 8 bits, 40x40 at 10 bits, 64x48 at 16 bits)
all 800 values are identical.

The readback grows from one `double` per 16x16 block to one per window, and
the host adds every window. Steady state, medians of nine interleaved pairs
of runs while other work loaded the host (load average 74 to 87):

| Run | Before | After |
|---|---|---|
| 1920x1080, default scale (4) | 2.31 ms | 2.45 ms |
| 1920x1080, `enable_lcs=true` | 2.36 ms | 2.58 ms |
| 3840x2160, default scale (8) | 5.86 ms | 5.86 ms |
| 3840x2160, `enable_lcs=true` | 5.98 ms | 6.40 ms |
| 1920x1080, `scale=1` | 19.6 ms | 22.0 ms |
| 1920x1080, `scale=1`, `enable_lcs=true` | 22.7 ms | 27.7 ms |
| 3840x2160, `scale=1` | 96.2 ms | 96.4 ms |
| 3840x2160, `scale=1`, `enable_lcs=true` | 101.0 ms | 119.5 ms |

At the default scale the scored plane is at most 480x270 and the change is
inside the spread between sets of runs (an earlier set gave 2.16 to 2.18 ms and
5.28 to 5.46 ms).

An explicit `scale=1` scores the full plane and pays
12 to 24 % at 1920x1080, depending on the set: 2.07 million windows are
16.6 MB per sum to read back (about 1.7 ms) and one chain of 2.07 million
host additions (about 2 ms).

Memory at `scale=1`: 8 bytes per window for each
sum on the device and in pinned host memory, 66 MB per sum at 3840x2160, four
sums with `enable_lcs=true`. The open tuning row is
`T-HIP-FLOAT-SSIM-EXACT-THROUGHPUT-2026-10-01` in
[`docs/state.md`](../../state.md).

## cambi_hip, speed_chroma_hip and speed_temporal_hip

`cambi_hip` ([ADR-1378](../../adr/1378-hip-cambi-device-resident.md)) and
`speed_chroma_hip` / `speed_temporal_hip`
([ADR-1384](../../adr/1384-hip-speed-device-resident.md)) no longer run any
stage of the CPU extractors on the host. Each frame is one staged upload (see
[Picture uploads](uploads.md#picture-uploads)), the whole pipeline on the
extractor's stream, and one small read of the result; `collect()` is the only
host wait. They port the SYCL designs of ADR-1357 and ADR-1358 and build for
gfx90a, gfx1030, gfx1036 and gfx1100.

On the gfx1036 `cambi_hip` matched the CPU on 178 of 178 frames,
`speed_chroma_hip` on
759 of 759 values and `speed_temporal_hip` on 256 of 256
([agreement table](overview.md#agreement-with-the-cpu)).

### Scores

- **`cambi_hip`** keeps `cambi.c`'s sliding column histograms and sums the
  top-K c-values exactly, so it scores bit-identically to `--backend cpu`
  wherever the CPU's own top-K sum is exact (every sub-4K frame measured on
  SYCL; the 4K BBB frames of the HIP sweep matched as well). It now refuses, as
  the CPU does, a window whose adjusted size exceeds
  65 x 65.
- **`speed_chroma_hip` / `speed_temporal_hip`** reproduce `speed.c` in fp32
  operation for operation. On HIP that takes build flags, not intrinsics: the
  kernel file is compiled with `-ffp-contract=off` and
  `-fhip-fp32-correctly-rounded-divide-sqrt`, because HIP's `__fmul_rn()`,
  `__fadd_rn()` and `__fdiv_rn()` are the plain (contracting) operators and
  `__fsqrt_rn()` is the approximate native square root. The device chain ends
  at the per-block variances; the host forms the entropies and the score from
  one block read back per frame, with `speed.c`'s own `log2()` calls
  ([ADR-1477](../../adr/1477-speed-upstream-double-math.md)). The scores
  equal the CPU's bit for bit on every build; see the
  [SpEED page](../../metrics/speed_qa.md#where-a-twin-computes-what).

### Requesting the twins

Request the twins by name; `--feature cambi` or `--feature speed_chroma` runs
the CPU extractor whatever `--backend` says:

```bash
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --no_prediction \
     --backend hip --feature cambi_hip --feature speed_chroma_hip \
     --feature speed_temporal_hip --json -o hip.json
```

Without a device, `test_hip_cambi_device_math` and `test_hip_speed_device_math`
replay every kernel on the host against the CPU extractors, and
`test_hip_device_resident_contract.py` checks the sources keep one upload, one
readback and one wait per frame. The on-device parity and timing commands are
in [`docs/state.md`](../../state.md) under
`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29` and
`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`.

### SpEED-chroma singularity (ADR-1202)

The HIP SpEED-chroma twin previously treated any non-zero return from its
linear-algebra helper as "singular covariance matrix" and imputed the `uv`
score from the other chroma channel. That rule is correct for the CPU
extractor, where a non-zero return does mean singular, but not here: this twin
handles singularity internally (warn, zero the solution, return 0) and uses
the return value for API errors only. A real device error was therefore fed
into the imputation, and with both chroma channels failing it averaged to
`0.0` and reported success.

Singularity now travels in its own `bool *singular_out` and hard errors
propagate, so a device failure inside SpEED-chroma fails the frame instead of
emitting a `0.0` score. The twin also adopts the CPU rule that a channel with
exactly one singular side (reference or distorted) scores 0 rather than an
inflated value. The launch-geometry half of ADR-1202 was CUDA-only — this
twin's solve launch was already correct.
