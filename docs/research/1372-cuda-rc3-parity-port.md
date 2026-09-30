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

### Options: where each acts

| Twin | Option | Where it acts on CUDA | Expected agreement with the CPU |
|---|---|---|---|
| `psnr_cuda` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | host, from the device SSE, `psnr_score.h` | bit-exact (integer SSE, same host arithmetic) |
| `integer_ssim_cuda` | `enable_db`, `clip_db` | host, `vmaf_ssim_max_db()` + `vmaf_ssim_emit_ratio_score_named()` | per-pixel terms bit-exact; the frame sum differs by a double rounding |
| `float_ssim_cuda` | `enable_lcs` | device: `calculate_ssim_vert_combine_lcs` reduces L, C, S per block in double; host rounds each mean to fp32 | the fp32 moments' difference (host replay: 6e-8 on 8-bit noise) |
| `float_ssim_cuda` | `enable_db`, `clip_db` | host, on the fp32-rounded mean | as above; identical flat frames exact |
| `float_motion_cuda` | `motion_max_val` | host, `motion_clip()` on motion2, debug motion and the flush tail | as the default score; capped frames exact |
| `motion_v2_cuda` | `motion_fps_weight`, `motion_max_val` (already declared) | host: the published SAD is `MIN(sad * mfw, mmxv)`, as the CPU's | bit-exact (integer SAD, same host arithmetic) |

### What the review of the first version found (#1637)

Two reviews of the first push, one of them replaying the arithmetic on the host, found five more defects. The replays quoted below are small C programs written for the review and the fix (they are not part of the tree): each reimplements the CPU reference and the kernel's arithmetic and reduction order from the sources named, compiled with `gcc -O2 -ffp-contract=off`. Each is fixed on the branch and pinned by a planted-regression case in `test_cuda_kernel_source_contract.py`:

- **`float_ssim_cuda` forced identical windows to exactly 1; the CPU does not.** The first version rounded the three products of its combined formula with `__fmul_rn` and returned 1 on numerator == denominator, on the assumption that the CPU scores identical windows exactly 1. It does not: `iqa/ssim_tools.c::ssim_accumulate_default_scalar` (and `iqa/ssim_accumulate_lane.h`, which the AVX2, AVX-512 and NEON variants share) computes `l = (2.0 * mu_r * mu_c + C1) / (float)(mu_r^2 + mu_c^2 + C1)` with a double numerator over an fp32 denominator, `c` the same way, `s` in fp32, and `iqa_ssim()` returns `(float)(ssim_sum / n)`. On a flat 128 frame the fp32 denominator rounds up by 0.0014 while the numerator stays exact, so `l = 0.99999995708`, the mean rounds to `0.99999994`, and the CPU reports 72.247199 dB where the first version reported `+inf` (and 88 dB, its `clip_db` ceiling, against 72.247 with `clip_db`). The fix computes each pixel with the CPU's types and rounding points (`ssim_score.cu::ssim_terms()`), reduces in double, and rounds the frame mean to fp32 on the host. A host replay (the CPU's double-accumulating separable convolution of `iqa/convolve.c` against the kernel's fp32 FMA convolution, then both combines and the kernel's reduction order) gives, for identical flat frames of 64x64 at 128, 64 (8-bit) and 512 (10-bit), CPU 72.247199 dB, new kernel 72.247199 dB, first version `+inf`; for identical noise frames `+inf` on all three at 64x64, 161x91 and 576x324; for distorted noise a difference of at most 6e-8, the fp32 moments' residue. The fp32 convolution was left as it is: accumulating it in double like the CPU would make the moments the CPU's too, for about 110 fp64 adds per pixel.
- **`integer_ssim_cuda` grouped its term as `w * (a * b / den)`; the CPU computes `((w * a) * b) / den`.** The two agree only where the window weight is a power of two; at 161x91, 841 of 1952 border pixels differ. The first version also compiled the combine with NVCC's default contraction: its sm_89 PTX has five `fma.rn.f64`, among them `c1 = sm^2 * K1 * w * w` rounded for the numerator but with its last `* w` fused into the denominator's sum. With `--fmad=false` the PTX has none (16 `mul.rn`, 17 `add.rn`, 3 `sub.rn` and the division) and the SASS of the combine drops from 26 to 17 `DFMA`, the rest being the correctly rounded division. With that and the CPU's grouping, a replay of the reviewer's sweep finds 0 per-term mismatches over 9,000,000 terms (1 to 24 pixels per side, 8 to 16 bits), and a second replay 0 over every term of 161x91, 576x324, 129x67, 17x17, 9x9, 5x3, 1x1, 4x23 and 1920x1080 frames at 8, 10, 12 and 16 bits. The frame sum still differs: the CPU adds the terms row by row, the kernel per warp, per block and on the host. On distorted noise that is at most 4.9e-14; on identical frames it matters only where the CPU's own sum is within a few ulps of 1, which a sweep of 57,600 identical frames of 1 to 24 pixels per side hit 121 times, 72 of them on opposite sides of 1 (`+inf` against about 156 dB without `clip_db`), none with both sides at 12 pixels or more. The single-pixel case (156.54 dB at 10 bits) is exact, as there is no sum to reorder.
- **`motion_v2_cuda` published the raw SAD.** The CPU publishes `MIN(sad * motion_fps_weight, motion_max_val)` and derives `motion2_v2` / `motion3_v2` from the stored value; the twin stored the raw SAD, weighted it in flush without the cap, blended the raw SAD into the `motion3_v2` stamp, and returned early for one frame where the CPU emits 0 / 0. At the default options all of this is invisible. The SYCL, HIP and Metal twins carry the same flush.
- **`psnr_cuda` was not `TEMPORAL`.** The CPU `psnr` is, so `--subsample N` still feeds it every frame; the twin was skipped on N - 1 of every N frames and its `apsnr_*` summed a subset. `psnr_sycl` has the same gap.
- **`psnr_cuda` zeroed its chroma accumulators on the readback stream** while the chroma kernels ran on the picture stream, with nothing ordering the two: the race `kernel_template.h` documents for the luma accumulator, which is zeroed on the picture stream. All three memsets now run on the picture stream.

`float_ssim_cuda`'s `enable_chroma` is not a CPU option and never did anything (the kernel only reads `data[0]`), but it was public, so it stays accepted and ignored (HISS-14) and logs a warning when set; `test_cuda_twin_option_parity` allows it as the one twin-only option.

The `.minnctapersm` advisory on the motion kernel (`__launch_bounds__(256, 8)`, above the thread limit of sm_86 / 89 / 120) only means ptxas ignores the minimum-blocks hint there. `-Xptxas -v` shows whether the 16-bit kernel's int64 vertical accumulator spills; the home steps add an `ncu` check.

## What was verified here, and what was not

Verified in `vmaf-dev-mcp:ocloc` (nvcc 13.4.92, gcc 15.2, no GPU):

- First version, on `origin/master` 905a989e3: full CUDA build (`-Denable_cuda=true -Denable_nvcc=true`, release, LTO) for sm_80 / 86 / 89 / 90 / 100 / 120 plus compute_80 / 120 PTX, every kernel compiled, no compiler warning; fast suite on that build 231 OK, 9 skipped (the CUDA device tests, exit 77), 1 failed: `test_gpu_picture_pool_uaf`, which the OOM killer stops under Meson's `MALLOC_PERTURB_` in the 12 GB Docker VM on `master` too (`T-GPU-POOL-UAF-OOM-ASAN-UBSAN-GAP-2026-06-06`) and which passes when run directly.
- Review fix pass, rebased on `origin/master` 044fac41c: every CUDA kernel, `libvmaf`, the CLI and the twelve CUDA tests this work adds or touches rebuilt with no compiler warning (the host crashed under build load several times that day, so the relink of every other test binary was skipped; the fix pass changes no CPU source). `-Xptxas -v` on the motion SAD kernels: no spill at sm_80 / 86 / 89 / 90 / 120, 28 to 40 registers; the `.minnctapersm` advisories (motion and ssimulacra2 kernels) predate this work and mean ptxas ignores the minimum-blocks hint where 8 x 256 threads exceed the SM, on sm_86 / 89 / 120.
- Device-free cases: `test_cuda_adm_dwt2_rows`; the option-table, twin-only-option (`enable_chroma`), unknown-option cases of `test_cuda_twin_option_parity`, the 2x3 / 3x2 / 2x2 rejections of `test_cuda_motion_tiny_frames` and the declaration and direct-rejection cases of `test_cuda_vif_min_dim` pass, and their device cases skip; `test_cuda_kernel_source_contract.py` passes, and each of its planted regressions is detected.
- The SSIM arithmetic: the PTX and SASS counts above, the host replays of both combines and reduction orders, and the CPU `ssim=enable_db=true` probe on identical frames.
- clang-format 23.1.1 on every touched C, CUDA and header file; a scoped CUDA-lane `tidy-ratchet.py --only` run (clang-tidy 22.1.2; the baseline was recorded with 22.1.8) over the 13 touched host and test TUs: none above its baseline, the new files at 0.

Not verified (needs an NVIDIA GPU): every CUDA score, the absence of device faults under `compute-sanitizer`, and the timing. The home steps are in the four rows of `docs/state.md`.

## References

- [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), [ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md), [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-0845](../adr/0845-cuda-motion-launch-overhead.md), [ADR-1199](../adr/1199-cuda-picture-handover-barrier.md).
