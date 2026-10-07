---
paths:
  - core/src/feature/hip/integer_psnr_hip.c
  - core/src/feature/hip/integer_ssim_hip.c
  - core/src/feature/hip/float_ssim_hip.c
  - core/src/feature/hip/float_motion_hip.c
invariant: Mirror CPU option tables faithfully across psnr, ssim, float_ssim, and float_motion.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CPU option tables on psnr / ssim / float_ssim / float_motion (ADR-1382)

- Option tables = CPU tables (names, aliases, types, defaults, ranges, flags).
  `test_hip_twin_option_parity` compares them device-free.
- `psnr_hip`: device reduces integer SSE only; host calls `psnr_score.h`
  (`vmaf_psnr_peak`, `vmaf_psnr_max`, `vmaf_psnr_from_mse`,
  `vmaf_psnr_aggregate`), `flush_fex_hip()` publishes `apsnr_*`. No local
  copy of PSNR math (`log10` in TU = regression).
- `integer_ssim_hip`, `float_ssim_hip`: `enable_db` / `clip_db` via
  `vmaf_ssim_max_db()` + shared SSIM emitters. `integer_ssim_hip`:
  `issim_pixel_term()` returns weight when factors equal (CPU raster sum
  absorbs quotient's ulp on large frames; per-block tree does not).
  Frames <= `ISSIM_HIP_RASTER_MAX_PIXELS` (4096): `integer_ssim_vert_terms`
  writes `issim_cpu_term()` per pixel at `y * width + x`, no identical rule;
  `collect()` adds in ascending index = CPU raster order -> score == CPU
  double (ADR-1400). Do not reduce these on device, reorder collect
  loop, or route them through `issim_pixel_term()`:
  `test_hip_ssim_tiny_frames` (`==`, device) and
  `test_hip_kernel_source_contract.py` fail.
- `float_ssim_hip` = CPU bits (ADR-1441, exact twin `float_ssim` /
  `float_ssim_lcs`; gfx1036: 178 of 178 frames, 712 values with
  `enable_lcs`). Window sums + terms = `integer_ms_ssim/ms_ssim_arith.h`,
  never copy: pass 1 `vmaf_hip_ms_ssim_horizontal(ref_taps, cmp_taps)`,
  pass 2 `vmaf_hip_ms_ssim_vertical()`, terms `vmaf_hip_ms_ssim_lcs(&m, c1,
  c2, c2 / 2.0f)`. No tap table, no `+=` of product, no `sqrtf` / `fmaf` in
  `float_ssim/ssim_score.hip`. fp32 running sum = rounds every tap, was up to
  4.8e-7 off (only `float_ssim_l` matched). Contraction off by build flag
  (ADR-1407). Cost of pair sums: +17 % 1080p, +7 % 4K at default scale,
  +32 % at `scale=1`.
- `float_ssim_hip`: no identical-window shortcut. CPU is 1 - 2^-24 on some
  identical frames (fp32 luminance denominator, 72.247 dB). `ssim_pixel()` =
  `(l * c) * s` in double; host `fssim_hip_cpu_mean()` rounds mean to fp32.
  Do not bring back combined Wang formula or `num == den ? 1`.
- `float_ssim_hip` frame sum = CPU raster order
  (`T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02`, ADR-1438 construction).
  Pass 2 stores one double per window at `y * w_final + x`, no `__shared__`,
  no shuffle, no block sum on device. Host `fssim_hip_frame_sums()` adds
  `i = 0 .. windows - 1` into one double per sum = `iqa/ssim_tools.c`
  `ssim_accumulate_default_scalar()`. Another order = another double -> mean
  one float step off on some frame (was `0xb4e2b621` vs CPU `0xb4e2b622` on
  pair in `core/test/float_ssim_order_frame.h`). fp32 rounding of mean
  does NOT absorb order. Fixture header shared by CUDA / HIP / SYCL:
  byte-identical, never edit. Guards: `test_hip_float_ssim_parity` (device,
  bits), `test_hip_kernel_source_contract.py` (device-free). Cost: readback
  8 bytes per window per sum (16.6 MB at 1080p `scale=1`), +4 ms there;
  default scale <= 480x270, in noise.
- `float_ssim_hip` scale > 1 (ADR-1405): `calculate_ssim_hip_decimate_{8,16}bpc`
  before pass 1 -> fp32 planes == CPU `iqa_decimate()` bit for bit. Window sum
  = `float_ssim/ssim_decimate.h` (`vmaf_hip_ssim_decimate_sample`): fp32
  `sample * tap`, int64 sum in 2^-52 units, one `(float)` round; symmetric
  period-2n mirror, not tile clamp. Plain C + HIP C++:
  `test_hip_float_ssim_decimate` compiles same lines vs `iqa_decimate()`.
  Keep one copy; no fp32 running sum, no double round. Tap
  `1.0f / (float)(scale * scale)` formed on host. Plane size
  `iqa_decimate_dim()`. Pass 1 reads raw samples at scale 1
  (`horiz_{8,16}bpc`), decimated planes above (`horiz_f32`); one template
  body. `check_context_hip()` refuses only decimated < 11x11 or scale > 128.
- `float_ssim_hip` `enable_lcs`: separate kernel
  `calculate_ssim_hip_vert_combine_lcs`, same `ssim_pixel()` (header's
  CPU types: clamped fp32 variances, double L/C, fp32 S, flat-window
  covariance clamp), three planes of per-window doubles `[l | c | s]` in
  `rb_lcs`, each in raster order; host adds four sums in one pass, each
  its own chain. Default kernel stays LCS-free.
- `float_motion_hip`: `motion_max_val` (`mmxv`); every emitted `motion` /
  `motion2`, debug and flush tail included, through `fm_hip_motion_clip()`.
- `float_motion_hip` option table == CPU `float_motion.c` table, same order
  (order spells feature names: `motion3_mbf_0.5_mbo_2`). ADR-1404:
  - `motion3` = `fm_hip_motion_blend_clip()` -> `motion_blend()` from shared
    `motion_blend_tools.h`. Index 0 from first SAD (frame 1's collect), then
    blended motion2 at `index - 1`, tail + one-frame 0 in `flush_fex_hip()`.
    No local blend math.
  - `motion_filter_size`: kernel arg, `fm_filter()` picks `FM_FILT` /
    `FM_FILT_3` / `FM_FILT_NO_OP`; 3-tap and no-op = 5 taps with zero outer
    weights (same tile, same halo). Min frame: 2x2 for mfs 3, else 3x3.
  - `motion_add_scale1`: `float_motion_hip_scale1_diff` after blur kernel
    of same plane, same stream, then row kernel.
    `vmaf_hip_float_motion_bilinear()` = `motion.c`
    `motion_bilinear_interp()` operand for operand, contraction off. Frame 0
    launches no SAD kernel; row sums zeroed at init.
  - `motion_add_uv`: `plane[3]`, each own `ref_in` + `blur[2]` + `diff[2]`;
    chroma dims via `vmaf_chroma_extent()` (ceil). One upload call for all
    planes, one read-back of every plane's row sums, one wait.
  - Upstream change to `motion_blur_plane()`, `vmaf_image_sad_c()`,
    `motion_scale_bilinear()` or `motion_blend_clip()` -> mirror here.
- `float_motion_hip` SAD = CPU's order, scores = CPU's bits with every option
  (ADR-1419, ADR-1409; gfx1036: 1617 of 1617 values). Rebase-sensitive:
  - All SAD arithmetic in `float_motion/float_motion_rows.h` (plain C + HIP
    C++): `abs_diff`, transposed index, `row_sum`, bilinear,
    `plane_score`. Kernels + extractor call it; no `__shfl_down`, no
    per-block partial, no host `double` sum.
  - Blur kernel stores `|cur - prev|` per sample in `diff[0]`, TRANSPOSED:
    index = `((y / 64) * width + x) * 64 + y % 64`. Row kernel
    `float_motion_hip_row_sum`: one thread per row, 64 per block
    (`FMH_ROW_THREADS` == `VMAF_HIP_FLOAT_MOTION_ROW_GROUP`), fp32 accumulator,
    left to right. Lanes of block read 64 consecutive floats per step.
  - Never read blurred planes in row kernel: one cache line per lane
    and step, 145 ms per 4K frame on gfx1036 (20 ms transposed, 18 before).
  - Host: `vmaf_hip_float_motion_plane_score()` = fp32 scale-0 mean (+ fp32
    scale-1 mean), planes added in double (`motion_score_pair()`).
  - `EXACT_TWINS["float_motion"]` lists `hip`: gate cell = equality.
  - Guards: `test_hip_float_motion_rows` (device-free vs `compute_motion()`),
    `test_hip_float_motion_parity` + `_large` (`==`, default and scale1 + uv),
    `test_hip_kernel_source_contract.py` (6 planted regressions).
- `motion_v2_hip` stores SAD as CPU: `MIN(score * mfw, mmxv)`; `motion2_v2` /
  `motion3_v2` come from CPU's `vmaf_motion_window_flush()` over stored
  values (ADR-1491): no re-weighting, one-frame run and end cases decided
  there. `test_hip_kernel_source_contract.py` pins call and that TU
  reads no stored score back.
- `motion_hip`: `debug` default false (CPU, CUDA); emits
  `VMAF_integer_feature_motion_sad_score` every frame, 0 at index 0 and under
  force_zero.
- gfx1036 (`ryzen-4090-arc`, ROCm 7.2.4) loses run of stream's commands
  about once per 10^4 frames, master too
  (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01). Symptoms: `vif_hip` frame =
  CPU sums of it + previous frame (lost accumulator memset), one scale 0/0
  (`invalid ratio`), scales 1-3 off; lone wrong `motion_v2_hip` SAD. Not
  twin bug: moving / replacing memset, host kernargs, polling, no direct
  dispatch all still fail. Check with `scripts/dev/hip_dispatch_drop_probe.hip`
  before chasing single-frame mismatch on that device; compare repeated
  runs.
- `motion_force_zero` (`motion_hip`, `float_motion_hip`): never set
  `submit` / `collect` to NULL in `init()`. Before #1637 libvmaf picked
  submit/collect from descriptor before `init()` ran, then called
  `fex->submit` after it: NULL there was SIGSEGV on frame 0
  (T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30). engine now runs
  `init()` first (`init_before_dispatch()`, `core/src/libvmaf.c`,
  T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30), so cleared pair
  would move twin to synchronous path instead; twins do not rely
  on that. Keep no-op `submit()` and `collect()` that writes
  `extract_force_zero()`'s zeros. Guard:
  `test_integer_motion_force_zero` in `test_hip_twin_option_parity`.
- `psnr_hip` TEMPORAL like CPU `psnr`: `--subsample` must not drop frames
  from `apsnr_*`. Twin's subsample flags (TEMPORAL / PREV_REF) follow CPU;
  `test_hip_twin_option_parity` checks.
- HIP error mapping: `vmaf_hip_rc_to_errno()` (`kernel_template.c`, declared in
  `core/src/hip/common.h`). No new private copies.
