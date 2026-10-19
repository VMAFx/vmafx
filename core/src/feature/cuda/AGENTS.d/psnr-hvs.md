---
paths:
  - core/src/feature/cuda/integer_psnr_hvs_cuda.c
  - core/src/feature/cuda/integer_psnr_hvs_cuda.h
invariant: PSNR-HVS participates in engine sync, honours enable_chroma, and matches CPU scores bit for bit.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# PSNR-HVS engine sync, chroma parity, and bit-exact scores

- **`integer_psnr_hvs_cuda.c` participates in engine-scope CUDA
  drain batch.** `submit_fex_cuda` queues one DtoH copy of term
  buffer (`s->rb`, 64 floats per block, ADR-1397) on `s->lc.str`, then
  `vmaf_cuda_kernel_submit_post_record`. `collect_fex_cuda` calls
  `vmaf_cuda_kernel_collect_wait` before reading `s->rb.host_pinned`;
  raw `cuStreamSynchronize` in collect reopens T-GPU-OPT-3's per-frame
  stall. No host copy, host conversion or upload of planes per frame
  (T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29); do not reintroduce one.
- **`integer_psnr_hvs_cuda.c` honours `enable_chroma` option parity** (mirrors
  ADR-0453 on psnr_hvs surface). `enable_chroma` (default `true`,
  ADR-1203) clamps `n_planes` to 1 when `false`; YUV400P always forces
  `n_planes = 1` (`configure_hvs_geometry`), as CPU extractor does.
  Plane loops iterate `s->n_planes`, not `PSNR_HVS_NUM_PLANES`; combined
  score is luma only when `n_planes == 1`. Loops that index per-plane array
  or header's offsets take count from `psnr_hvs_plane_count()`
  (`n_planes` capped at `PSNR_HVS_NUM_PLANES`), so bound is array's.
- **`integer_psnr_hvs/psnr_hvs_score.cu` reads raw device samples**
  (CUDA port of ADR-1369). Two threads per 8x8 block (even thread =
  reference, odd = distorted), raw 8- to 12-bit samples via picture
  pitch (`hvs_load_block`; no depth scaling, T-CUDA-PSNR-HVS-ODD-BPC-2026-09-30),
  DCT in shared memory, `__shfl_xor_sync` swaps variance ratio + masking
  energy, reference thread stores block's terms. Tail threads run
  block 0 and never store, so whole warp reaches shuffle.
- **`psnr_hvs_cuda` = CPU scores bit for bit (ADR-1397).** Kernel
  stores 64 terms `calc_psnrhvs()` sums per block (`hvs_store_terms`,
  row-major, blocks in plane then raster order); host hands each plane
  to `vmaf_psnr_hvs_plane_score()` (`../psnr_hvs_score.c`: one running
  `float`, CPU order), combined score + dB via same file. Load-bearing,
  each one breaks bit-identity on its own (Research-1397 §3):
  - masking table = `(csf * 0.3885746225901003)^2` in `double`, stored
    `float` (`hvs_mask_value`, constexpr -> static data);
  - threshold = upstream's statement (ADR-1488): `product = energy * ratio`
    in `float` (plain operator, one rounding under `--fmad=false`), then
    `(float)(sqrt((double)product) / 32.0)` (`hvs_threshold`). Not
    `(double)energy * (double)ratio`: that exact product was fork's CPU
    between PR #552 and ADR-1488, one float step off upstream on about one
    block in twenty;
  - `--fmad=false` on fatbin (every fatbin has it, ADR-1403:
    `cuda_device_strict_fp_args` in `core/src/meson.build`);
  - coefficient error = integer `abs()` cast to `float`;
  - no sum of terms in kernel or host TU (per-block partials round
    differently: 1e-2 dB at 3840x2160).
  Guards: `test_cuda_psnr_hvs_parity{,_large}` (device, `==` on all four
  outputs, 3840x2160 included; cases live in
  `core/test/psnr_hvs_twin_parity.h`, shared with SYCL and HIP twins,
  ADR-1401), `test_psnr_hvs_twin_exact_sum_contract.py`
  and `test_psnr_hvs_score` (device-free). Gate cell = tolerance 0
  (`EXACT_TWINS`, `scripts/ci/cross_backend_calibration.py`). Upstream
  change to `calc_psnrhvs()` arithmetic or order -> mirror it here in
  same PR. Readback = 256 bytes per block (65 MB per 3840x2160
  4:2:0 frame); tuning tracked as
  T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01, must stay bit-exact.
- **Module load = `psnr_hvs_load_module()`** (context pushed, module +
  four entry points `psnr_hvs`, `hvs_scan_reduce`, `hvs_scan_prefix`,
  `hvs_compact` resolved, context popped on every path). new kernel entry
  point is resolved there, not in `init_fex_cuda()`.
