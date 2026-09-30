<!-- markdownlint-disable MD013 MD060 -->
# Research-1372: CUDA RC3 parity port — motion order, CPU options, tiny-frame guards

- **Status**: Active
- **Workstream**: RC3 CUDA; `T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`, `T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29` (CUDA half), `T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29` (CUDA half), `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` (CUDA part); decisions in [ADR-1372](../adr/1372-cuda-motion-diff-first-pipeline.md), [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md), [ADR-1374](../adr/1374-cuda-integer-tiny-frame-guards.md)
- **Last updated**: 2026-09-30

## Question

The SYCL twins were brought to the CPU's arithmetic and options on 2026-09-29 (ADR-1371, ADR-1365, the tile clamp and VIF fallback of `fix/sycl-b580-psnr-hvs-adm-tiny`). Which of those defects does the CUDA backend share, what is the smallest change that reproduces the CPU there, and what can be shown without an NVIDIA device?

## Sources

- CPU references: `core/src/feature/integer_motion.c` (`motion_score_pipeline_8` / `_16`, `extract()`, `flush()`), `integer_motion_v2.c`, `float_motion.c` (`motion_clip()`), `integer_psnr.c` + `psnr_score.h`, `integer_ssim.c`, `float_ssim.c` + `iqa/ssim_tools.c`, `integer_adm.c`, `integer_vif.c` / `integer_vif.h`.
- CUDA twins at `origin/master` f5dff7b58: `integer_motion_cuda.c` + `integer_motion/motion_score.cu`, `integer_motion_v2_cuda.c` + `integer_motion_v2/motion_v2_score.cu`, `integer_psnr_cuda.c`, `ssim_cuda.c`, `integer_ssim_cuda.c` + `integer_ssim/ssim_score.cu`, `float_motion_cuda.c`, `integer_adm_cuda.c` + `integer_adm/adm_dwt2.cu`, `integer_vif_cuda.c` + `integer_vif/filter1d.cu`; engine: `libvmaf.c` (`cuda_order_pictures_against_producer`, `read_pictures_extractor_loop_cuda`, `flush_context_cuda`).
- SYCL references: `integer_motion_pipeline_sycl.cpp`, `integer_psnr_sycl.cpp`, `integer_ssim_sycl.cpp`, `float_motion_sycl.cpp`, `integer_vif_sycl.cpp`; [Research-1371](1371-sycl-motion-diff-first-pipeline.md), [Research-2127](2127-sycl-twin-cpu-option-parity.md), [Research-2123](2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md).
- Host: Windows 11 + WSL2, `vmaf-dev-mcp:ocloc` (CUDA 13.4 nvcc, gencode sm_80 / 86 / 89 / 90 / 100 / 120 + compute_80 / 120 PTX). **No NVIDIA device**: every CUDA device test skips (exit 77) here, and every number below that needs a GPU is taken from the SYCL measurements, not from CUDA.

## Findings

### Motion: the same order defect, and the fix already existed in the tree

`motion_score.cu` wrote each blurred frame to a uint16 ping-pong and summed `|blur(cur) - blur(prev)|`; the CPU sums `|blur(prev - cur)|` with a rounding shift after each pass. This is the arithmetic `motion_sycl` had until ADR-1371, where it cost up to 2.0e-4 at 17x17 and 1.26e-5 on the Netflix 576x324 pair; the CUDA twin was not measured (no device) and is expected to show the same numbers.

`motion_v2_score.cu` already computed the CPU's order, and the CPU `motion_v2` pipeline is character for character the CPU `motion` pipeline (`motion_score_pipeline_8` / `_16`: same diff, same `1 << 7` / `1 << (bpc - 1)` vertical rounding, same `>> 16` horizontal). So the port reuses that kernel for `motion_cuda` rather than writing a third implementation: the SAD a frame pair produces is by construction the one `motion_v2_cuda` produces, which the existing `test_cuda_motion_v2_parity` compares with the CPU. `integer_motion_sad_cuda.c` owns the module, the packed-luma staging copy, the accumulator reset and the launch; the two extractors keep their own post-processing (`motion_cuda`: eight-frame batch readback and host motion2 / motion3 of ADR-0845 / ADR-0219; `motion_v2_cuda`: per-frame readback and flush-time motion2_v2 / motion3_v2).

Three more differences surfaced while reading the host code:

- **Debug score.** `emit_batch_scores` emitted `cur_score`, the raw normalised SAD, as `VMAF_integer_feature_motion_score`; `integer_motion.c::extract` emits `MIN(sad / 256 / (w * h) * motion_fps_weight, motion_max_val)`. Only visible with `debug=true` and a non-default weight or cap. Now the CPU's value; `test_cuda_motion_tiny_frames` pins it with `debug=true:motion_fps_weight=0.6` (keys `integer_motion_mfw_0.6` / `integer_motion2_mfw_0.6` / `integer_motion3_mfw_0.6`, checked against the CPU run in the container).
- **Ping-pong ordering.** Frame *i*'s kernel reads the slot frame *i*-1 filled on another picture stream, and frame *i*+1's copy overwrites the slot frame *i*'s kernel read. Nothing in either extractor ordered those: both relied on the engine's per-frame `cuCtxSynchronize` (ADR-1199), which was added for pictures filled by an external producer. The shared submit now waits on the previous frame's event (`motion_cuda`: `event`; `motion_v2_cuda`: `lc.submit`) on the device before it copies — no host wait, and correct if the ADR-1199 barrier is ever narrowed.
- **Readback.** The batch boundary synchronised the readback stream, queued the eight copies, and synchronised again. The copies already queue behind every chained frame event, so the first wait was redundant; `motion_readback_slots()` does one, and the flush tail shares it.

Tile halo: `motion_v2_score.cu` reflected every tile index once, including the padding threads'. At width 17 the second block's tile spans x = 14..33 and `mv2_mirror(33, 17) = -1`, a read one sample before the row (before the buffer on row 0). Same class as `T-SYCL-TILE-HALO-OOB-READ-2026-09-29`; the reflected index is now clamped into the plane (`cuda_tile_index.h`), the identity for every consumed sample.

### Integer ADM vertical DWT: no escape from the ADM minimum up

`adm_dwt2_load_column()` is used by the scale-0 fused kernel only; scales 1-3 run `dwt_s123_combined_*` with `calculate_indices()`. Replaying the launched grid (`DIV_ROUND_UP((h + 1) / 2, 8)` block rows of two thread rows, each loading 10 source rows from `2 * y_out - 1`) for every height up to 8192 (`test_cuda_adm_dwt2_rows`, from the kernel's own header):

| Plane height | Rows outside the plane (bare reflection) |
|---|---|
| 1 to 8 | yes (the SYCL defect's range) |
| 9 to 16 | none (below the ADM minimum anyway) |
| 17 (ADM_MIN_FRAME_DIM) to 8192 | none |

The farthest row a thread loads is `16 * grid_rows`, which stays below `2h` once `h >= 9`. For scales 1-3, output *n* reads `2n - 1 .. 2n + 2` reflected once at `upper`; every launched output is below `(upper + 1) / 2`, which keeps each tap in `[0, upper)` for `upper >= 2`, and the ladder from 17 rows ends at 3 rows at scale 3. So the CUDA half of the row is not affected at supported sizes. The clamp added to the scale-0 load is the identity there (the test checks that explicitly) and keeps the kernel in bounds on its own. The arithmetic now lives in `integer_adm/adm_dwt2_rows.h`, which the kernel, its launch and the test share, with `static_assert`s tying the kernel instantiation to the header's geometry. `compute-sanitizer` on a device remains the confirming step.

### Integer VIF: in bounds below 16, but not the CPU's values

Unlike the SYCL loaders, `filter1d.cu` already clamps after its reflection ("Safety clamp for very narrow frames"), so a sub-16 frame does not read outside the buffers. But a tap that needs a second reflection is clamped to the edge instead, which is not what the CPU computes, so `vif_cuda` would score such frames differently from the CPU without any signal. The minimum is the one `vif_sycl` derives: `max((w_s / 2 + 1) << s)` over the scale filters {17, 9, 5, 3} and the decimation filters {9, 5, 3} = 16. `vif_cuda` computes it from `vif_filter1d_width` (`integer_vif.h`, the tables its kernels use) and declares it through the ADR-1324 gate.

### Options: where each acts, and why identical SSIM windows need uncontracted products

| Twin | Option | Where it acts on CUDA | Expected agreement with the CPU |
|---|---|---|---|
| `psnr_cuda` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | host, from the device SSE, `psnr_score.h` | bit-exact (integer SSE, same host arithmetic) |
| `integer_ssim_cuda` | `enable_db`, `clip_db` | host, `vmaf_ssim_max_db()` + `vmaf_ssim_emit_ratio_score_named()` | as the linear score, mapped through the dB slope |
| `float_ssim_cuda` | `enable_lcs` | device: `calculate_ssim_vert_combine_lcs` reduces L, C, S per block; host sums in double | fp32 per-pixel terms (the SYCL twin measured 8.3e-7) |
| `float_ssim_cuda` | `enable_db`, `clip_db` | host | as above |
| `float_motion_cuda` | `motion_max_val` | host, `motion_clip()` on motion2, debug motion and the flush tail | as the default score; capped frames exact |

Identical frames and `enable_db`: `float_ssim.c` reports `+inf` (or the `clip_db` ceiling) because its score is exactly 1. The CUDA combine did not reach 1. The SASS of master's `calculate_ssim_vert_combine` (sm_89) shows why: NVCC fused the variances into `FFMA R12, -R37, R37, R12` (`ref_sq - ref_mu * ref_mu` with the product unrounded) and the denominator's mean sum into `FFMA R7, R14, R14, R7`, while the covariance kept `FMUL R37, R14, R37` then `FADD R4, R4, -R37`. For identical windows the numerator and denominator therefore differ in the last bits. With the three products rounded by `__fmul_rn`, the new SASS has three `FMUL`s and only `FADD`s against them (the remaining `FFMA`s are `2 * x + c`, exact doubling), so both sides run mirrored operations and the kernel returns exactly 1. The integer twin evaluates the CPU's per-pixel double expression in source, but `integer_ssim_score.cu` was the one SSIM kernel built with NVCC's default contraction (its HIP twin builds with `-ffp-contract=off`). The master PTX of `integer_ssim_vert_combine` (sm_89) has five `fma.rn.f64`: `2 * mxy + c1` in the numerator adds the rounded `c1 = sm^2 * K1 * w * w`, while the denominator's `mux^2 + muy^2 + c1` fuses the last `* w` of `c1` into its add, and `c2` and `y2 * w` are fused the same way. So the combine was not the CPU's expression at any bit depth, and an identical window compared a rounded `c1` against an unrounded one. Replaying the master PTX op for op on the host (`Fraction`-exact FMAs) over identical noise frames, the two sides still came out equal at 8 and 10 bits and the frame reduced to exactly 1, but by the size of the rounding, not by construction. With `--fmad=false` the PTX has no `fma.rn.f64` (16 `mul.rn`, 17 `add.rn`, 3 `sub.rn`, the division) and the sm_89 SASS of the combine drops from 26 to 17 `DFMA` (the rest belong to the correctly rounded division); numerator and denominator of an identical window are then the same operations on the same values at 8 and 10 bits, where every moment product is an integer below 2^53 (the widest, `2 * 1023^2 * 2^32`, is 8.99e15). At 12 and 16 bits even the CPU's numerator and denominator differ for 3.1% and 25.8% of identical noise windows (161x91), yet the CPU reports `+inf` for identical 4:4:4 noise at 8, 10, 12 and 16 bits (161x91, 576x324, 853x480), and the host replay of the kernel's arithmetic and reduction order (16x8 blocks, warp shuffle tree, host sum) gives exactly 1 for identical noise at all four depths and three sizes. That is agreement on the fixtures, not a proof, for 12 and 16 bits.

`float_ssim_cuda`'s `enable_chroma` was a leftover: the CPU `float_ssim` has no such option, the kernel only reads `data[0]`, and `test_twin_options_are_cpu_options` now fails for any CUDA twin option that is not the CPU's.

## What was verified here, and what was not

Verified in `vmaf-dev-mcp:ocloc` (nvcc 13.4.92, gcc 15.2, no GPU), on the branch rebased onto `origin/master` 905a989e3:

- Full CUDA build (`-Denable_cuda=true -Denable_nvcc=true`, release, LTO) for sm_80 / 86 / 89 / 90 / 100 / 120 plus compute_80 / 120 PTX: every touched kernel compiles and no compiler warning is emitted. The ptxas `.minnctapersm` advisories of the motion and ssimulacra2 kernels predate this change; the motion one now fires for one kernel pair instead of two.
- Fast suite on that build: 231 OK, 9 skipped (the CUDA device tests, exit 77), 1 failed: `test_gpu_picture_pool_uaf`, which the OOM killer stops under Meson's `MALLOC_PERTURB_` in this 12 GB VM on `master` too (`T-GPU-POOL-UAF-OOM-ASAN-UBSAN-GAP-2026-06-06`) and which passes when run directly.
- Device-free cases: `test_cuda_adm_dwt2_rows` 6/6; the option-table, twin-only-option and unknown-option cases of `test_cuda_twin_option_parity`, the 2x3 / 3x2 / 2x2 rejections of `test_cuda_motion_tiny_frames` and the declaration and direct-rejection cases of `test_cuda_vif_min_dim` pass, and their device cases skip; `test_cuda_kernel_source_contract.py` passes with each of its 14 planted regressions detected.
- The integer SSIM combine: the PTX and SASS counts above, the host replay of its arithmetic, and the CPU `ssim=enable_db=true` probe on identical frames.
- clang-format 23.1.1 on every touched C, CUDA and header file; a scoped CUDA-lane `tidy-ratchet.py --only` run (clang-tidy 22.1.2; the baseline was recorded with 22.1.8) over the 13 touched host TUs, where no file exceeds its baseline.

Not verified (needs an NVIDIA GPU): every CUDA score, the absence of device faults under `compute-sanitizer`, and the timing. The home steps are in the four rows of `docs/state.md`.

## References

- [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-0845](../adr/0845-cuda-motion-launch-overhead.md), [ADR-1199](../adr/1199-cuda-picture-handover-barrier.md).
