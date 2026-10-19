---
paths:
  - core/src/feature/cuda/float_motion_cuda.c
  - core/src/feature/cuda/float_motion_cuda.h
  - core/src/feature/cuda/float_motion/float_motion_score.cu
invariant: float_motion emits CPU motion3 and executes SAD in bit-exact CPU order.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_motion CPU parity and SAD order

- **`float_motion_cuda` emits CPU `motion3`**
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`): `motion_blend_clip()` =
  CPU `float_motion.c::motion_blend_clip` (fps weight, blend, cap) of
  `motion2`; frame 0 from first SAD at index 1, tail from `flush`, `0`
  for one-frame input. `motion_blend_factor` / `motion_blend_offset`
  declared in CPU table order (order spells feature names, e.g.
  `motion3_mbf_0.5_mbo_2`). `flush` appends through
  `motion_append_once()` (probe, then append) because pending collect
  may already have written tail.
- **`float_motion_cuda` SAD = CPU order, bit for bit (ADR-1409).**
  `float_motion.c::compute_motion_simd()` = one fp32 running sum per row
  (`float_sad_line*`, all SIMD twins sequential), one fp32 sum over rows,
  fp32 division. Twin: `float_motion_row_sad` = ONE thread per row, plain
  `for (j = 0; j < width; j++)` loop, readback `frame_h` floats; host =
  `vmaf_float_motion_score_from_row_sads()` (`../float_motion_sad.h`). No
  block / warp / atomic reduction in `float_motion_score.cu`, no host sum
  in `float_motion_cuda.c`: any other shape differs in low bits (was
  1.36e-4 on 1080p checkerboards). Blur kernels write blur only; blur =
  `convolution_f32_c_s()` tap order, needs `--fmad=false` (ADR-1403). CPU
  SAD order changes upstream -> change kernel + helper in same PR. RTX
  4090: Netflix pair 48 / 48, checkerboards 3 / 3, BBB 4K 200 / 200
  identical, all three outputs; 4K time unchanged (3.00 -> 2.98 ms).
  `EXACT_TWINS` lists `float_motion`: `cuda`. SYCL / HIP / Metal twins
  still per block: `T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01`.

- Tile loads index through `vmaf_cuda_tile_index(vmaf_cuda_reflect_101(idx,
  sup), sup)` (`cuda/cuda_tile_index.h`), as HIP's `fm_tile_index()`. Bare
  reflect-101 sent padding loads of plane 3-9 or 17 samples wide / high to
  negative index: read before plane, into memory no output uses
  (T-CUDA-FLOAT-MOTION-TILE-READ-BEFORE-PLANE-2026-10-05). Scores unchanged.
  Guard: `test_cuda_kernel_source_contract.py`
  (`test_unclamped_float_motion_tile_mirror_is_detected`).
