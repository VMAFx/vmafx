- **Preallocate pinned host pictures for zero-copy 4K CLI CUDA upload (ADR-1406).**
  The `vmaf` CLI now preallocates pinned host pictures via `VmafPicturePool` when
  running CUDA-accelerated feature extractors, eliminating the synchronous driver
  bounce buffer copy on pageable host memory. At 3840×2160 on RTX 4090, default
  model frame time drops from 5.42 ms to 4.87 ms/frame (10.1% faster), `psnr_cuda`
  from 2.29 ms to 2.10 ms/frame (8.3% faster), `adm_cuda` from 3.99 ms to 3.65 ms/frame
  (8.5% faster), and `vif_cuda` from 2.32 ms to 1.82 ms/frame (21.6% faster). Scores
  remain bit-identical with zero drift across all features. If pinned memory allocation
  fails, the pool transparently falls back to pageable memory.
