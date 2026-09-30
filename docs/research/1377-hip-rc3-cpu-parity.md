<!-- markdownlint-disable MD013 MD060 -->
# Research-1377: HIP RC3 CPU parity — motion order, tiny-frame reads, CPU options

- **Status**: Active
- **Workstream**: RC3 HIP port of `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29`, the HIP halves of `T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29` and `T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`, and the HIP part of `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26`; decisions in [ADR-1377](../adr/1377-hip-motion-diff-first.md), [ADR-1381](../adr/1381-hip-integer-tiny-frame-guards.md) and [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)
- **Last updated**: 2026-09-30

## Question

The SYCL twins were brought to CPU parity on real Intel hardware (ADR-1365, ADR-1371, #1622). Which of those defects do the HIP twins share, what is the smallest change that gives them the CPU's arithmetic, and what can be established without an AMD device?

## Sources

- CPU references: `core/src/feature/integer_motion.c`, `integer_motion_v2.c`, `float_motion.c`, `integer_psnr.c` with `psnr_score.h`, `integer_ssim.c`, `float_ssim.c` with `iqa/ssim_tools.c`, `integer_vif.h`, `adm_csf_fixed_point.h`.
- HIP twins: `core/src/feature/hip/` (`integer_motion_hip.c`, `integer_motion_v2_hip.c`, `integer_motion/motion_score.hip` before its removal, `integer_motion_v2/motion_v2_score.hip`, `integer_adm/adm_dwt2.hip`, `integer_adm_hip.c`, `integer_vif_hip.c`, `integer_vif/vif_statistics.hip`, `integer_psnr_hip.c`, `integer_ssim_hip.c` and its kernel, `float_ssim_hip.c` and its kernel, `float_motion_hip.c`); `core/src/hip/picture_hip.c`.
- SYCL designs: [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [Research-2123](2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md).
- Host: `vmaf-dev-mcp:ocloc` (ROCm hipcc, HIP 7.15), a full `-Denable_hip=true -Denable_hipcc=true` build for gfx90a, gfx1030, gfx1036 and gfx1100, and a scaffold build (`-Denable_hipcc=false`). No AMD GPU: device tests skip (exit 77).

## Findings

### Motion: the HIP twin blurred each frame; the shared kernel is the CPU's SAD

`integer_motion/motion_score.hip` blurred each frame into a uint16 ping-pong and summed `|blur(cur) - blur(prev)|`, the pre-PR #532 order that ADR-1371 removed from `motion_sycl` (2.0e-4 at 17x17, 1.26e-5 on the Netflix pair; the HIP overview records the same 1.26e-5 on `motion2` / `motion3` on a gfx1036). `motion_v2_score.hip` already differenced first and rounds like the CPU. The two CPU extractors compute the same SAD: `integer_motion_sad` of `motion` against `integer_motion_v2_sad_score` of `motion_v2`, frame by frame with `--precision=max`, differ by exactly 0 on the 576x324 fixture (48 frames, SIMD and `--cpumask 255` scalar) and on random 4:4:4 clips of 3x3, 17x17, 19x23, 33x33 and 257x145 at 8 and 10 bits (5 frames each). So running `motion_hip` on the `motion_v2` kernel gives it the CPU `motion` SAD, and the host scoring (motion2 = min of neighbours, motion3 = blend, clip, moving average) was already the CPU's.

The debug `motion` score was the unweighted SAD score where the CPU emits `MIN(score * motion_fps_weight, motion_max_val)`; a one-frame run never wrote `motion3[0]`, which the CPU reports as the stamp value 0.

### Uploads: the only host wait was the copy of the picture

Every HIP extractor stages its host pictures with `vmaf_hip_picture_upload()`, which waits until the copy has read the pageable picture (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). Copying the luma into an extractor-owned pinned buffer on the host reads the picture before `submit()` returns, so the device copy can run later from the buffer without a wait. Reusing the buffer next frame is safe because `dispatch_gpu_double_buffer()` collects frame N - 1 (whose `collect()` drains the private stream) before it submits frame N; `test_hip_upload_race` drives exactly that submit / refill / collect sequence.

### Tile loads: one reflection leaves small planes for padding threads

`motion_v2_score.hip` loads a 20x20 tile per 16x16 block, halo included, for every thread. On a 17-sample axis the last block's halo index 33 reflects to `2 * 17 - 33 - 2 = -1`. A host replay of every block and tile slot (`test_hip_adm_dwt2_rows`, extents 3 to 1024) shows `vmaf_hip_tile_index(vmaf_hip_reflect_101(i, n), n)` always lands in `[0, n)` and equals the bare reflection for every slot an output consumes.

`float_motion_score.hip` has the same geometry (16x16 blocks, 5-tap filter, 20x20 tile) and its own single reflection, `fm_mirror()`. Review of #1636 replayed its launch: extents 3 to 9 and 17 load outside `[0, n)` (index range [-13, 2] at 3, [-1, 16] at 17), i.e. before `ref_in`'s allocation, pre-existing since the float-motion port. Its loads now use the same clamped index, and the replay above covers it.

### ADM scale-0 vertical DWT: rows escape only below 9 rows

`adm_dwt2_load_column()` serves the fused scale-0 kernel only; scales 1 to 3 run `dwt_s123_combined_vert_kernel_*`, which reads per output row and stays inside from 2 rows up. Replaying every thread row of the scale-0 launch (`DIV_ROUND_UP((h + 1) / 2, 8)` block rows of 2 thread rows, 10 source rows per thread) for every height from 1 to 8192: the single reflection leaves the plane for heights 1 to 8 exactly and never from 9 up; `integer_adm_hip` refuses frames below 17x17, so no accepted frame read outside before the change either. The clamp is the identity from 9 rows up.

### VIF: the 16-pixel bound, and why HIP did not fault

`floor(dim / 2^s)` must exceed the tap half-width of each scale filter {17, 9, 5, 3} and of the decimation filter {9, 5, 3} of scales 0 to 2: needs {9, 10, 12, 16} and {5, 6, 8}, bound 16, the same number `vif_sycl` uses. HIP's `mirror2_i()` clamps after the reflection, so unlike SYCL it did not read outside its buffers, but below 16 pixels it read other samples than the CPU, and below 8 pixels scale 3 is empty.

### Integer SSIM: the CPU's per-pixel term, and identical windows

`issim_pixel_term()` evaluates the CPU's double expression operand for operand with contraction off (ADR-0564), so its per-pixel terms are the CPU's doubles and only the summation order differs. For an identical window the numerator and denominator factors are equal (exactly, while the moments are exact in double: 8- and 10-bit input) and the CPU's quotient `((w * f) * g) / (f * g)` is `w` up to an ulp; for random `f`, `g` and integer `w <= 65536` it misses `w` in 36.7 % of 200000 draws. The CPU's raster-order running sum absorbs those residues (the CPU reports `+inf` with `enable_db` on identical 576x324, 17x17 and 257x145 frames at 8 and 10 bits); a per-block tree need not. Returning the weight itself for an identical window makes identical frames exactly 1 on the twin without changing any other term. A host replay in review of #1636 (the CPU `calc_ssim()` body against an emulation of the kernel and collect order) confirms it from 3x3 up; on 1x1 and 2x2 identical frames the CPU sum has one to four terms, does not absorb the ulp, and reports 156.54 and 159.55 dB, where the twin reports `+inf` (`T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30`). Matching those needs the CPU's sequential sum on the device.

### Float SSIM: the CPU is not exactly 1 on every identical frame, and the twin now follows it

With `enable_db` on identical frames the CPU `float_ssim` reports `+inf` on the synthetic fixtures and on 17x17 / 257x145 noise, but on some frames of the 576x324 fixture, and on a flat 64x64 frame of 128, 72.247 dB: one fp32 ulp below 1 (`1 - 2^-24`). The cause is in `iqa/ssim_tools.c`: the luminance term divides a double numerator, `2.0 * mu_r * mu_c + C1`, by a denominator formed in fp32, `mu_r * mu_r + mu_c * mu_c + C1`. For a flat 128 frame that is 32774.50250 over the fp32 32774.50391, so `l = 1 - 4.3e-8`; C and S are exactly 1; the mean `(float)(sum / (w * h))` then rounds `1 - 4.3e-8` to the nearer fp32, `1 - 2^-24`. The first draft of this port forced an identical window to exactly 1 (ADR-1365's SYCL form), which reports `+inf` there, against the CPU.

The twin now reproduces the CPU arithmetic instead. Pass 2 forms each pixel's term as `ssim_accumulate_default_scalar()` does, `(l * c) * s` in double from `ssim_lcs()` (the CPU's types: clamped fp32 variances, one fp32 `sqrtf` of the variance product, L and C in double over fp32 denominators, S in fp32, `C3 = C2 / 2`, contraction off), reduces one double per block, and the host rounds the frame mean to fp32. HIP's default fp32 division and square root are correctly rounded, so no equal-operand guard is needed for identical windows, unlike the SYCL device's approximate ones. This also removes the combined-formula residual that `float_ssim_sycl` still carries (`T-SYCL-FLOAT-SSIM-COMBINED-FORMULA-RESIDUAL-2026-09-29`: up to 7.8e-5 with the combined formula, within 9e-7 with the product form on SYCL). Bit-exact dB on every identical frame also needs the vertical moments to equal the CPU's, which depends on contraction in the moment passes (the separate fp-contract work).

### What runs without a device

| Check | Where | Result here |
|---|---|---|
| Full HIP build, every touched kernel for gfx90a / gfx1030 / gfx1036 / gfx1100 | `ninja -C build-hip` | builds, no new warnings; `clang-offload-bundler --list` shows all four code objects in `motion_v2_score`, `ssim_score`, `integer_ssim_score` and `adm_dwt2`, and `llvm-nm` every kernel entry point, `calculate_ssim_hip_vert_combine_lcs` included |
| Scaffold build (`enable_hipcc=false`) | `ninja -C build-scaffold` | builds, no warnings in touched files |
| Row and tile replays | `test_hip_adm_dwt2_rows` | pass |
| Source contract with 26 planted regressions | `test_hip_kernel_source_contract.py` | pass; each regression detected; the review-fix rules fail on the unfixed sources |
| Option tables equal the CPU's (`motion_hip`'s `debug` default included), `--subsample` flags and `motion_hip`'s feature set follow the CPU, ADR-1183 gate passes, unknown key refused | `test_hip_twin_option_parity` (first three tests) | pass |
| Parity gate: HIP backend, `float_ssim_lcs` cell, every HIP cell names a registered extractor | `scripts/ci/test_cross_backend_parity_gate.py` | pass |
| vif_hip bound, CPU fallback declaration, direct init `-EINVAL` | `test_hip_vif_min_dim` (first two tests) | pass |
| Motion twins refuse frames below 3x3 | `test_hip_motion_tiny_frames` (first test) | pass |
| Device parity | the same three tests, remaining cases | skip (77) |
| CPU fast suite in the HIP build | `run_meson_test.py -- -C build-hip --suite=fast` | 226 OK, 14 skipped (HIP device tests), 1 timeout: `test_gpu_picture_pool_uaf`, whose 32 GiB allocation succeeds under `vm.overcommit_memory=1` in this container and is then filled by `MALLOC_PERTURB_`; it passes when run without `MALLOC_PERTURB_`, and neither it nor the pool code is touched by this change |
| clang-tidy 22.1.8, HIP lane, touched TUs | `tidy-ratchet.py --lane hip --only ...` | every touched file at or below `tidy-baseline-hip.json`; new files clean |
| CPU `motion` SAD against CPU `motion_v2` SAD | `vmaf --feature motion` / `--feature motion_v2`, `--precision=max`, `testdata/ref_576x324_48f.yuv` | identical on all 48 frames |

### Device results (gfx1036, 2026-10-01)

On `ryzen-4090-arc` (Ryzen 9950X3D iGPU, gfx1036, ROCm 7.2.4, Linux 7.2.8), built with `-Dhip_gfx_targets=gfx1036`:

| Check | Command | Result |
|---|---|---|
| Row tests | `run_meson_test.py -- -C build-hip --num-processes 1 test_hip_motion_tiny_frames test_hip_motion_parity test_hip_motion3_parity test_hip_motion_v2_parity test_hip_upload_race test_hip_adm_dwt2_rows test_hip_adm_tiny_frames test_hip_adm_parity test_hip_adm_small_border test_hip_vif_min_dim test_hip_vif_parity test_hip_twin_option_parity test_hip_psnr_parity test_hip_float_ssim_parity test_hip_float_motion_parity` | 15 OK, no skip marker, no `Memory access fault` |
| Whole device suite | `run_meson_test.py -- -C build-hip --suite gpu --num-processes 1` | 52 OK; one skip marker, `test_hip_float_ssim_parity_large` (`float_ssim_hip` does not decimate) |
| `motion_hip` vs CPU `motion` | `--feature motion`, `--precision=max`, Netflix pair | `motion2` / `motion3` 1.26e-5 apart on master, identical on the branch; identical on all 9600 frames of 20 looped runs |
| Parity gate | `cross_backend_parity_gate.py ... --features float_ssim float_ssim_lcs psnr motion_v2 vif` | 1.0e-5, 1.0e-5, 0, 0, 1.0e-6, all OK |
| `psnr_hip` options | `psnr=enable_mse=true:enable_apsnr=true:reduced_hbd_peak=true:min_sse=0.5` | per-frame values and `apsnr_*` identical |
| `float_motion_hip` 3x3 / 17x17 | row `T-HIP-FLOAT-MOTION-TILE-OOB-2026-09-30` | exit 0, within 4e-6 / 1e-6; master does not fault either, so the host replay stays the evidence |
| `motion_force_zero` | `--feature motion_hip=motion_force_zero=true` | master exits 139 (NULL `submit()`); fixed, `T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30` |

Two findings came out of the device run. First, the test expectations of `test_hip_float_motion_parity` (tail motion2) and the one-frame case of `test_hip_twin_option_parity` (collector names) were stale; the twins were right. Second, the gfx1036 loses stream commands: a probe (`scripts/dev/hip_dispatch_drop_probe.hip`) that zeroes 12 slots, launches 12 kernels that each write one and count themselves, and reads the slots back, lost 382 of 6.0 million dispatches over five runs of 100000 frames, always a run of commands from the start of a frame (the memset and the first 1 to 11 kernels). That is the source of the rare `vif_hip` frames that carry the previous frame's sums, lose a scale's sums, or go wrong from scale 1 on, which also appear on master; moving or replacing the accumulator memset changes nothing, and `HIP_FORCE_DEV_KERNARG=0`, `HSA_ENABLE_INTERRUPT=0`, `AMD_DIRECT_DISPATCH=0` or a spinning wait do not stop it (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`).

Throughput, BBB 4K, `(median t(22) - median t(2)) / 20` over 3 interleaved runs: `motion_hip` 14.25 ms/frame on master and 12.95 on the branch, `motion_v2_hip` 10.17 and 13.24. The `motion_v2_hip` kernel alone is 9.76 and 10.03 ms (hipEvent timing of the two code objects on the same 4K planes), so nearly all of the difference is the staged upload: with `vmaf_hip_picture_upload()` in its place the twins run at 10.70 and 10.75 ms/frame. On this iGPU the runtime maps a pageable picture and copies it by SDMA without a host copy (about 0.3 ms), while the staged path first copies the luma into pinned memory on the host with the GPU idle, because libvmaf collects frame N - 1 before it submits frame N. With several twins in one process, and with the `vmaf_v0.6.1` model (ADM on the CPU), the two uploads are within noise.

## Alternatives explored

- A diff-first kernel of its own for `motion_hip` instead of the shared one: rejected, two copies of one arithmetic (HISS-19); the CUDA branch shares its kernel the same way.
- ADR-1365's fp32 formula for `integer_ssim_hip`: rejected, it would replace per-pixel terms that are already the CPU's doubles.
- `-ffp-contract=off` for all of `ssim_score.hip`: rejected here in favour of a pragma on the per-pixel formula; build-wide contraction is the separate fp-contract port.

## Open questions

- Answered on 2026-10-01 (see Device results): device confirmation on the gfx1036, and the throughput effect of the staged upload there (a loss for a single motion twin at 4K).
- Whether the staged upload pays off on a discrete AMD GPU, where the runtime stages pageable copies itself; no such device was available.
- Whether a ROCm or amdgpu update stops the gfx1036 from losing stream commands (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`, probe command in the row).

## Related

- [ADR-1377](../adr/1377-hip-motion-diff-first.md), [ADR-1381](../adr/1381-hip-integer-tiny-frame-guards.md), [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md); SYCL: [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [Research-2127](2127-sycl-twin-cpu-option-parity.md).
