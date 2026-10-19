# SYCL twin notes

A SYCL twin is the device implementation of a CPU feature extractor. This page
says how the twins reach the CPU's bits and gives the current behaviour of each
family. Which twins are exact, and since which ADR, is in the
[agreement table](overview.md#numerical-agreement-with-the-cpu); the dated
measurements behind each statement are in [History](history.md).

## What the SYCL compile line guarantees

Under icpx every SYCL feature kernel is compiled with
`-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`
([ADR-1367](../../adr/1367-sycl-strict-fp-every-feature-tu.md)). Each fp32 `+`,
`-`, `*`, `/` and `sqrt` in a kernel then rounds the way the CPU reference build
rounds it: IEEE-754 round to nearest, one rounding per operation, `a * b + c`
never fused into an FMA, subnormal results kept.

This holds for the native images built for the `sycl_icpx_aot_targets` devices
and for the SPIR-V image compiled at first launch on any other device, and
`test_sycl_fp_arith_contract` checks it on the device. A Windows MSVC build
generates every image, native and SPIR-V, in its one explicit device link
([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md)), which gets the
same flags; that path has not been measured on a GPU yet. A kernel that needs a
fused multiply-add says so with `sycl::fma()`.

`-fp-model=precise` on its own does not give this: it leaves `a * b + c` fused
and `/` and `sqrt` approximate (29 % and 8 % of random fp32 operands differ from
the host).

AdaptiveCpp builds accept neither `-fp-model` nor the precision flags. They
compile with contraction off, and division and square root keep the backend's
default precision.

## What a twin handles beyond the compile line

The line alone does not make scores bit-identical. A twin has to handle four
further things:

- **Transcendental functions.** `sycl::log2`, `exp`, `pow`, `cbrt`, `sin`
  and `atan2` are not correctly rounded on the device and are not the host's
  libm. `float_vif_sycl` evaluates the CPU's `log2` polynomial instead
  (ADR-1422), and `ciede_sycl` its own functions on pairs of `float` values
  (ADR-1436,
  [history](history.md#ciede_sycl-follows-the-cpu-ciede-to-14e-11-2026-10-01)).
- **Summation order.** A work-group reduction adds in a tree, not in the
  CPU's sequential order. The twins add integers where the terms allow it
  (`float_psnr`, `float_moment` up to $2^{53}$ units), reproduce the CPU's
  sequential `double` sum from integer increments where they do not
  (`ssimulacra2`, ADR-1446; `float_moment` past $2^{53}$ units, ADR-1497),
  or read the terms back and add them on the host in the CPU's order
  (`ssim`, ADR-1443; `float_ssim`, ADR-1463; `float_ms_ssim`, ADR-1466).
- **fp64 on the CPU.** SYCL kernels are fp32-only
  ([ADR-0220](../../adr/0220-sycl-fp64-fallback.md)); where the CPU evaluates
  an expression in fp64, the twin carries it as an exact pair of floats
  (SpEED, `float_adm`) or computes the fp64 value in 64-bit integers
  (`ssim`, `float_ssim`, `float_ms_ssim`, `ssimulacra2`).
- **Content the gate's fixtures do not have.** The repository's 10-, 12- and
  16-bit clips are 8-bit content shifted left. `float_moment_sycl` and
  `float_psnr_sycl` matched them and differed on full-range noise until
  ADR-1449 and ADR-1450.

Two device-side controls reduce the remaining deviation:

- **fp16 is off for scoring.** Some Intel GPUs expose fp16 arithmetic; libvmaf
  forces fp32 on the kernel so scores are portable across hosts with different
  fp16 rounding modes.
- **Work-group reductions use a fixed iteration order**, which removes the most
  common source of cross-run drift (a non-deterministic reduction tree).

## Per-family notes

### ADM (`adm_sycl`, `float_adm_sycl`)

- `adm_sycl` honours `adm_csf_mode` (all four CSF models) and `adm_p_norm`.
  Its option table mirrors the CPU table entry for entry, so the keys it emits
  match the CPU's for any options dictionary. The default model requests
  `adm_csf_mode=2` under the key
  `integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.
- `aim_score` and `adm3_score` come from the device
  ([ADR-1362](../../adr/1362-sycl-integer-adm-aim-device-pass.md)), and the twin
  accepts `adm_skip_aim`. The HIP twin has one too since
  [ADR-1525](../../adr/1525-adm-hip-aim-device-pass.md).
- Every ADM output equals the CPU's, except with a non-integer
  `adm_enhn_gain_limit`; the shipped models use 1.0 or 100.
- `float_adm_sycl` keeps the CPU's division and takes its CSF weights, region
  and pooling from the CPU routines
  ([ADR-1434](../../adr/1434-sycl-float-adm-cpu-arithmetic.md)).
- Measurements: [ADM
  history](history.md#integer_adm_sycl-and-the-default-models-adm-2026-09-05),
  [float_adm history](history.md#float_adm_sycl-matches-the-cpu-float_adm-exactly-2026-10-02).

### VIF (`vif_sycl`, `float_vif_sycl`)

- `vif_sycl` needs frames of at least 16x16. When libvmaf picks the twin itself
  from a model's VIF features, a smaller frame is computed by the CPU `vif`
  extractor with the log line
  `feature extractor 'vif_sycl' cannot honour WxH; computing 'vif' on the CPU`.
  Naming the twin (`--feature vif_sycl`) fails instead with
  ``vif_sycl requires width >= 16 and height >= 16``. The CUDA and HIP twins
  carry the same check; Metal does not yet.
- Odd plane widths (854x480, 1366x768, 853x480) are scored correctly.
- The `debug` option defaults to `false`, as on the CPU; ask for the eleven
  debug outputs with `--feature vif_sycl=debug=true`.
- `sycl::mul_hi()` on 64-bit operands returned wrong values on an Arc A380, so
  the twins form wide products in 32-bit limbs
  (`core/src/feature/sycl/sycl_soft_double.h`).
- `float_vif_sycl` takes the CPU's Gaussian taps from `vif_get_filter()`, uses
  the CPU's `log2f_approx()` polynomial, and accepts `vif_scale1_min_val`,
  `vif_scale2_min_val` and `vif_scale3_min_val`.

### Motion (`motion_sycl`, `motion_v2_sycl`, `float_motion_sycl`)

- `motion_sycl` and `motion_v2_sycl` difference the two frames first and blur
  the difference, as the CPU does
  ([ADR-1371](../../adr/1371-sycl-motion-diff-first-pipeline.md)).
- `motion_sycl` writes `VMAF_integer_feature_motion_sad_score` on every frame
  and honours `motion_force_zero`: it returns 0 for every output and computes no
  SAD.
- `motion_five_frame_window=true` runs on the device on `motion_sycl` and
  `motion_v2_sycl`, with the CPU's bits
  ([ADR-1491](../../adr/1491-gpu-motion-five-frame-window.md)). `motion_sycl`
  copies one more luma plane per frame for it and refuses the option together
  with its own `motion_add_uv` (`-ENOTSUP`: the CPU has no chroma mode to
  match).
- `motion_add_uv=true` on `motion_sycl` stages U and V in pinned host memory and
  uploads them on the motion queue, so the frame keeps one wait, in `collect()`.
- `motion_max_val` clips both `motion2` and `motion3`; `motion_v2_sycl` applies
  `motion_fps_weight` and the cap in `collect()` and derives `motion2_v2` and
  `motion3_v2` in `flush()`, including for one-frame inputs.
- `float_motion_sycl` adds the SAD per row in the CPU's order
  ([ADR-1411](../../adr/1411-sycl-float-motion-cpu-float-sum.md)) and emits
  `motion`, `motion2` and `motion3`. It declares `debug`, `motion_force_zero`,
  `motion_fps_weight`, `motion_blend_factor` (`mbf`), `motion_blend_offset`
  (`mbo`) and `motion_max_val` (`mmxv`); the three options it does not declare
  are listed under [Known gaps](overview.md#known-gaps).
- Measurements:
  [motion](history.md#motion_sycl-matches-the-cpu-motion-exactly-2026-09-29),
  [float_motion](history.md#float_motion_sycl-matches-the-cpu-float_motion-exactly-2026-10-01),
  [motion3](history.md#float_motion_sycl-emits-motion3-2026-10-03).

### PSNR family (`psnr_sycl`, `float_psnr_sycl`, `float_moment_sycl`, `psnr_hvs_sycl`)

- `psnr_sycl` takes `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and
  `min_sse`, and carries the `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag so
  `--subsample` scaling works as for the other temporal extractors.
- `float_psnr_sycl` and `float_moment_sycl` add integers, so the sum is exact in
  any order; `float_moment_sycl` reproduces the CPU's rounding past $2^{53}$ units
  ([ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md)).
- `psnr_hvs_sycl` stores the 64 terms of every block and the host adds them in
  the CPU's order, so it equals the CPU at every frame size and at 8 to 12 bits
  ([ADR-1401](../../adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). The CPU takes
  its masking threshold as a `float` product, which the kernel, having no fp64,
  reproduces with integer arithmetic. The price is a readback of 256 bytes per
  block (65 MB per 3840x2160 frame) and a sequential host sum; timings are on
  [the psnr_hvs page](../../metrics/psnr-hvs.md#gpu-twins).
  4:0:0 input is scored on luma only.
- `psnr`, `psnr_hvs` and `motion_v2` read the shared uploaded frame instead of
  uploading their own copies; chroma goes up once per frame, only when a twin
  reads it ([ADR-1369](../../adr/1369-sycl-shared-planes-light-twins.md)).
- `psnr_hvs` at 9 and 11 bits scores the raw sample, as the CPU does.

### SSIM family (`integer_ssim_sycl`, `float_ssim_sycl`, `float_ms_ssim_sycl`)

- `integer_ssim_sycl` runs the CPU's fp64 operations on values held in 64-bit
  integers and stores one term per pixel; the host adds the plane in raster
  order ([ADR-1443](../../adr/1443-sycl-ssim-cpu-arithmetic.md)). It takes
  `enable_db` and `clip_db`.
- `float_ssim_sycl` decimates on the device at every `float_ssim` scale
  ([ADR-1370](../../adr/1370-sycl-float-ssim-device-decimation.md)), takes
  `enable_lcs`, `enable_db` and `clip_db`, and adds the CPU's terms in the CPU's
  order ([ADR-1463](../../adr/1463-sycl-float-ssim-raster-sum.md)). The CPU
  extractor runs, with the usual warning, only when the reduced picture is
  smaller than SSIM's 11x11 window.
- `float_ms_ssim_sycl` enqueues the pyramid in `submit()`, waits once in
  `collect()`, and adds each scale's terms on the host in the CPU's order
  ([ADR-1466](../../adr/1466-sycl-float-ms-ssim-raster-sum.md)). At 4K it is
  slower than a 16-thread CPU run, so `--backend cpu` is faster there.
- Both SSIM twins score identical windows exactly 1, as the CPU does, so
  `enable_db` reports `+inf` and `clip_db` the CPU's ceiling.
- Measurements:
  [ssim](history.md#integer_ssim_sycl-matches-the-cpu-ssim-exactly-2026-10-02),
  [float_ssim decimation](history.md#float_ssim-decimation-on-the-device-2026-09-29),
  [float_ssim order](history.md#float_ssim_sycl-adds-its-frame-sums-in-the-cpus-order-2026-10-02),
  [float_ms_ssim](history.md#float_ms_ssim_sycl-computes-the-cpus-arithmetic-2026-10-01).

### SSIMULACRA 2 (`ssimulacra2_sycl`)

The twin is device-resident
([ADR-1363](../../adr/1363-sycl-ssimulacra2-msssim-device-resident.md)): one
upload of the raw planes, then colour conversion, XYB, blurs, SSIM and edge sums
and downsample on the device, and one 864-byte readback per frame. The six
per-sample terms are the CPU's doubles in 64-bit integers, and the sums follow
the CPU's order ([ADR-1446](../../adr/1446-sycl-ssimulacra2-cpu-bits.md)). See
[SSIMULACRA 2](../../metrics/ssimulacra2.md#sycl-device-resident-one-readback-per-frame).

### CAMBI (`cambi_sycl`)

The twin runs every stage on the device, reads the distorted plane from the
shared frame upload and reads back one 88-byte block per frame
([ADR-1357](../../adr/1357-sycl-cambi-device-resident.md)). At 3840x2160 a frame
takes 9.3 ms on an Arc B580 and 42 ms on a UHD 770. It supports
`cambi_high_res_speedup`. Per-frame scores are bit-identical to `--backend cpu`
whenever the CPU's own top-K double sum is exact, and otherwise differ by that
sum's rounding (2.2e-15 at most over 50 frames of Big Buck Bunny 4K). See
[the CAMBI metric page](../../metrics/cambi.md#sycl).

### CIEDE2000 (`ciede_sycl`)

The kernel runs the CPU's statements with every `double` as a pair of floats and
every math-library call as a function on such pairs; the host adds the
read-back plane in raster order
([ADR-1436](../../adr/1436-sycl-ciede-cpu-arithmetic.md)). It is the only gated
twin that is not exact: the host's `powf` rounds differently on 18 to 64 values
per 3840x2160 frame, which keeps it within 1.4e-11 of the CPU. The gate allows
`1e-9`.

### SpEED (`speed_chroma_sycl`, `speed_temporal_sycl`)

Both twins upload the raw planes once per frame, run filtering, covariance,
eigenvalues and the QR solve on the device as one recorded SYCL graph, and read
one result block per frame. The host forms the entropies and the score with
`speed.c`'s own `log2()` calls
([ADR-1477](../../adr/1477-speed-upstream-double-math.md)), so the scores equal
`--backend cpu` on an icx and on a GCC CPU
([ADR-1358](../../adr/1358-sycl-speed-device-resident-linalg.md)). Timings and
the parity check are on [the SpEED
page](../../metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu).

Each covariance entry is the CPU's sequential fp64 sum: the differences and
products are formed in parallel, then one work-item per entry adds them in
raster order, so the stored covariance matches `speed.c` bit for bit on every
frame, 4K and larger included
([ADR-2690](../../adr/2690-sycl-speed-covariance-fast-exact.md)).

`speed_chroma_sycl` reports a singular covariance separately from a device
failure: a failure fails the frame instead of emitting `0.0`, and a channel
with exactly one singular side scores 0 as on the CPU
([ADR-1202](../../adr/1202-cuda-speed-chroma-4k-launch-bounds.md)).

### Device faults

When the device reports a fault, the graph extractors (`adm_sycl`, `vif_sycl`,
`motion_sycl`, `psnr_sycl`, `float_moment_sycl`) and `psnr_hvs_sycl` return
`-EIO` for the frame, and the CLI stops with an error after a line such as
`libvmaf ERROR SYCL graph wait: level_zero backend failed with error: 20
(UR_RESULT_ERROR_DEVICE_LOST)`. Frames 64 rows high or less no longer lose the
device.
