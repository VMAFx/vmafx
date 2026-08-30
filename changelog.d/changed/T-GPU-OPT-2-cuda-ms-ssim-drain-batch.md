- **`integer_ms_ssim_cuda` joins the engine-scope CUDA fence-batching
  helper (`drain_batch`).** Previously the extractor host-blocked 6
  times per frame: one `cuStreamSynchronize` at the end of `submit()`
  and five inside `collect()` (one per pyramid scale, forced by a
  shared partials buffer). The PR allocates **per-scale** partials
  buffers (5× `l_partials[]` / `c_partials[]` / `s_partials[]` device
  + matching pinned host shadows), enqueues all 5 SSIM scales' `horiz`
  + `vert_lcs` launches and DtoH copies back-to-back on
  `s->lc.str` inside `submit()`, records `s->lc.finished` once after
  the last DtoH, and calls `vmaf_cuda_drain_batch_register(&s->lc)` so
  the engine's `vmaf_cuda_drain_batch_flush` waits on the lifecycle
  alongside the rest of the CUDA feature stack. `collect()` becomes
  a host-side reduction only — `vmaf_cuda_kernel_collect_wait`
  short-circuits when the engine has already drained the lifecycle.
  Bit-exact (same kernels, same stream, same submission order; only
  the host wait point moves; cross-backend `places=4` gate unchanged).
  Expected ms_ssim wall-clock improvement on the Netflix CUDA
  benchmark: +3-5%. See [ADR-0271](docs/adr/0271-cuda-drain-batch-ms-ssim.md)
  and the per-frame syscall profile in
  [research-0061](docs/research/0061-cuda-ms-ssim-drain-batch-profile.md).
