- **SYCL `psnr` chroma extension** (`psnr_sycl` now emits `psnr_y`,
  `psnr_cb`, `psnr_cr`) — second port of T3-15 GPU coverage long-tail
  batch 4 (T3-15(b)). Sibling to the CUDA twin in PR #520 / commit
  7f3d58a5; mirrors the Vulkan twin shipped in PR #204 /
  [ADR-0216](docs/adr/0216-vulkan-chroma-psnr.md) and the CPU contract
  at [`core/src/feature/integer_psnr.c`](core/src/feature/integer_psnr.c).
  Chroma rides on per-extractor device buffers populated by host-side
  staging copies in the graph `pre_fn` (the existing SYCL shared frame
  buffer is luma-only by design); luma stays graph-recorded, chroma
  kernels run direct in `post_fn` on the same in-order combined queue.
  Cross-backend `cross_backend_vif_diff.py --feature psnr --backend sycl
  --places 4` clears bit-exactly on Intel Arc A380 (0/48 mismatches on
  the Netflix normal pair, all three planes; `max_abs_diff = 0.0`).
