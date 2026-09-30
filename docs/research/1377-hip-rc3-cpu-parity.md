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

### ADM scale-0 vertical DWT: rows escape only below 9 rows

`adm_dwt2_load_column()` serves the fused scale-0 kernel only; scales 1 to 3 run `dwt_s123_combined_vert_kernel_*`, which reads per output row and stays inside from 2 rows up. Replaying every thread row of the scale-0 launch (`DIV_ROUND_UP((h + 1) / 2, 8)` block rows of 2 thread rows, 10 source rows per thread) for every height from 1 to 8192: the single reflection leaves the plane for heights 1 to 8 exactly and never from 9 up; `integer_adm_hip` refuses frames below 17x17, so no accepted frame read outside before the change either. The clamp is the identity from 9 rows up.

### VIF: the 16-pixel bound, and why HIP did not fault

`floor(dim / 2^s)` must exceed the tap half-width of each scale filter {17, 9, 5, 3} and of the decimation filter {9, 5, 3} of scales 0 to 2: needs {9, 10, 12, 16} and {5, 6, 8}, bound 16, the same number `vif_sycl` uses. HIP's `mirror2_i()` clamps after the reflection, so unlike SYCL it did not read outside its buffers, but below 16 pixels it read other samples than the CPU, and below 8 pixels scale 3 is empty.

### Integer SSIM: the CPU's per-pixel term, and identical windows

`issim_pixel_term()` evaluates the CPU's double expression operand for operand with contraction off (ADR-0564), so its per-pixel terms are the CPU's doubles and only the summation order differs. For an identical window the numerator and denominator factors are equal (exactly, while the moments are exact in double: 8- and 10-bit input) and the CPU's quotient `((w * f) * g) / (f * g)` is `w` up to an ulp; for random `f`, `g` and integer `w <= 65536` it misses `w` in 36.7 % of 200000 draws. The CPU's raster-order running sum absorbs those residues (the CPU reports `+inf` with `enable_db` on identical 576x324, 17x17 and 257x145 frames at 8 and 10 bits); a per-block tree need not. Returning the weight itself for an identical window makes identical frames exactly 1 on the twin without changing any other term.

### Float SSIM: the CPU is not exactly 1 on every identical frame

With `enable_db` on identical frames the CPU `float_ssim` reports `+inf` on the synthetic fixtures and on 17x17 / 257x145 noise, but on some frames of the 576x324 fixture 72.247 dB: one fp32 ulp below 1 (`1 - 2^-24`), a residue of its fp32 `l * c * s` and the final float cast. The mirrored twin reports `+inf` (or the `clip_db` ceiling) for every identical frame, as the SYCL twin does. No device arithmetic reproduces the CPU's residue; the ADR-1221 contract (perfect score is `+inf`) is what both twins implement.

### What runs without a device

| Check | Where | Result here |
|---|---|---|
| Full HIP build, every touched kernel for gfx90a / gfx1030 / gfx1036 / gfx1100 | `ninja -C build-hip` | builds, no new warnings; `clang-offload-bundler --list` shows all four code objects in `motion_v2_score`, `ssim_score`, `integer_ssim_score` and `adm_dwt2`, and `llvm-nm` every kernel entry point, `calculate_ssim_hip_vert_combine_lcs` included |
| Scaffold build (`enable_hipcc=false`) | `ninja -C build-scaffold` | builds, no warnings in touched files |
| Row and tile replays | `test_hip_adm_dwt2_rows` | pass |
| Source contract with 15 planted regressions | `test_hip_kernel_source_contract.py` | pass; each regression detected |
| Option tables equal the CPU's, ADR-1183 gate passes, unknown key refused | `test_hip_twin_option_parity` (first two tests) | pass |
| vif_hip bound, CPU fallback declaration, direct init `-EINVAL` | `test_hip_vif_min_dim` (first two tests) | pass |
| Motion twins refuse frames below 3x3 | `test_hip_motion_tiny_frames` (first test) | pass |
| Device parity | the same three tests, remaining cases | skip (77) |
| CPU fast suite in the HIP build | `run_meson_test.py -- -C build-hip --suite=fast` | 226 OK, 14 skipped (HIP device tests), 1 timeout: `test_gpu_picture_pool_uaf`, whose 32 GiB allocation succeeds under `vm.overcommit_memory=1` in this container and is then filled by `MALLOC_PERTURB_`; it passes when run without `MALLOC_PERTURB_`, and neither it nor the pool code is touched by this change |
| clang-tidy 22.1.8, HIP lane, touched TUs | `tidy-ratchet.py --lane hip --only ...` | every touched file at or below `tidy-baseline-hip.json`; new files clean |
| CPU `motion` SAD against CPU `motion_v2` SAD | `vmaf --feature motion` / `--feature motion_v2`, `--precision=max`, `testdata/ref_576x324_48f.yuv` | identical on all 48 frames |

## Alternatives explored

- A diff-first kernel of its own for `motion_hip` instead of the shared one: rejected, two copies of one arithmetic (HISS-19); the CUDA branch shares its kernel the same way.
- ADR-1365's fp32 formula for `integer_ssim_hip`: rejected, it would replace per-pixel terms that are already the CPU's doubles.
- `-ffp-contract=off` for all of `ssim_score.hip`: rejected here in favour of a pragma on the per-pixel formula; build-wide contraction is the separate fp-contract port.

## Open questions

- Device confirmation on the maintainer's gfx1036: bit-exact `motion_hip`, PSNR options, SSIM dB on identical frames, `enable_lcs` within 5e-4, VIF fallback below 16 pixels. Commands in the four `docs/state.md` rows.
- Throughput effect of the staged upload on gfx1036 (`(t(22) - t(2)) / 20` on BBB 4K, in the motion row).

## Related

- [ADR-1377](../adr/1377-hip-motion-diff-first.md), [ADR-1381](../adr/1381-hip-integer-tiny-frame-guards.md), [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md); SYCL: [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [Research-2127](2127-sycl-twin-cpu-option-parity.md).
