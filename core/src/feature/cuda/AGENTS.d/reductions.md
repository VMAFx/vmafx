---
paths:
  - core/src/feature/cuda/integer_adm_cuda.c
  - core/src/feature/cuda/integer_psnr_cuda.c
invariant: CUDA reduce kernels use warp-reduce plus block-atomic additions.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CUDA warp reduction and atomicAdd invariants

- **Every CUDA reduce kernel SHOULD use warp-reduce + `atomicAdd_int64`
  into single accumulator; separate per-thread scratch buffer plus
  separate reduce kernel launch = pre-fix legacy pattern.**
  Scale 0 of ADM CM (`adm_cm_line_kernel_8` in `integer_adm/adm_cm.cu`)
  = canonical model: compute per-pixel result, warp-reduce
  int64 accumulator, first lane atomicAdds into `accum_global`.
  Scales 1-3 migrated to this pattern by `i4_adm_cm_line_kernel_fused`
  (PR perf/adm-cm-cuda-warp-reduce-fusion). Any future reduce kernel
  writing to scratch buffer and launching second kernel to sum it
  should refactor to fused pattern instead.

## Integer reductions: one atomic per block (ADR-1392)

- **`motion_v2_score.cu` and `psnr_score.cu` add ONE `atomicAdd` per
  block** to frame's single 64-bit accumulator: warp shuffle, warp sums
  through `__shared__`, first warp sums them. `integer_moment/moment_score.cu`
  adds one per accumulator per block (threads 0..3 each sum one accumulator's
  warp sums; geometry in `integer_moment_cuda.h`, as PSNR's). Per-warp atomics
  to one address serialised kernels on L2 atomic unit (PSNR spent most
  of its GPU time there). Sums are integers, so any order is exact; never
  return to per-warp or per-thread atomics on single address.
- **Motion SAD vertical pass once per block** into `VTile s_v` (16 output
  rows x 20 tile columns, CPU `>> bpc` rounding), then 5 horizontal taps
  (`>> 16`) per output: same integers as per-output nested loop.
  `__launch_bounds__(256, 6)`: 6 x 256 = 1536 threads of sm_86 /
  89 / 120 SM (8 exceeded it: ptxas ignored hint with
  `.minnctapersm` and capped sm_80 / 90 at 32 registers).
- **PSNR geometry lives in `integer_psnr_cuda.h`** (`PSNR_BLOCK_X` x
  `PSNR_BLOCK_Y` threads, `PSNR_COLS_PER_THREAD` columns per thread,
  `PSNR_BLOCK_X` apart so warp loads stay contiguous); kernel and
  `psnr_cuda_dispatch()` must both read it.
- **Never index by-value `VmafPicture` kernel parameter with runtime
  plane** (`pic.data[plane]`): nvcc copies both pictures (192 bytes) to
  every thread's stack. `psnr_score.cu::plane_row()` selects with constant
  indices; `cuobjdump --dump-resource-usage` must show `STACK:0`.

## Accumulator reset runs on the stream of the kernels that add into it (T-UPSTREAM-1305-CUDA-VIF-ACCUM-STREAM-2026-10-01)

- Rule: `cuMemsetD8Async` of accumulator + kernels that `atomicAdd` into it
  = same stream. Streams order only within themselves.
- Audited same rule, no change needed: motion SAD (`integer_motion_sad_cuda.c`),
  `integer_adm_cuda.c` (all on `s->str`), `float_adm_cuda.c`,
  `float_psnr_cuda.c`, `integer_psnr_cuda.c`, `integer_cambi_cuda.c`,
  `kernel_template.h`.
- Guard: `test_cuda_multi_instance` (4 instances, shared `CUcontext`, scores `==`
  single instance; fails without fix on every run).
