- **`psnr_cuda` chroma extension — `psnr_cb` / `psnr_cr` on CUDA
  (T3-15(b), [ADR-0351](../docs/adr/0351-cuda-chroma-psnr.md)).**
  Extends the luma-only [ADR-0182](../docs/adr/0182-gpu-long-tail-batch-1.md)
  CUDA PSNR extractor to emit `psnr_cb` / `psnr_cr` alongside
  `psnr_y`, mirroring [ADR-0216](../docs/adr/0216-vulkan-chroma-psnr.md)'s
  Vulkan port. Three-element readback array in `PsnrStateCuda`
  (`rb[3]`) carries per-plane device-SSE accumulators + pinned-host
  slots; the kernel (`calculate_psnr_kernel_{8,16}bpc` in
  `core/src/feature/cuda/integer_psnr/psnr_score.cu`) gains a
  `plane` parameter so it indexes `data[plane] / stride[plane]`
  instead of the hard-coded `[0]`. A single private stream +
  submit/finished event pair issues all per-plane launches
  back-to-back on the picture stream (no inter-plane barrier — the
  accumulators are independent), then DtoHs all three slots on
  `lc.str` before a single `cuStreamSynchronize` in `collect()`.
  YUV400P clamps `n_planes = 1` so chroma dispatches and emits
  are skipped. `picture_cuda` upload path needed no change —
  chroma planes were already uploaded for non-`YUV400P` inputs
  since the `ciede_cuda` landing (`libvmaf.c::translate_picture_host`'s
  `upload_mask`). Cross-backend gate
  (`scripts/ci/cross_backend_vif_diff.py --feature psnr --backend
  cuda`) covers all three plane scores at `places=4`; RTX 4090
  measurement on the 576×324 *and* 640×480 testdata fixtures
  reports `max_abs_diff = 0.0` across 48 frames for every metric
  (deterministic int64 SSE on both sides). Closes the GPU
  long-tail backlog row "psnr chroma parity with CPU" across both
  shipping GPU backends; chroma SSIM / chroma MS-SSIM CUDA
  follow-ups stay separate rows.
