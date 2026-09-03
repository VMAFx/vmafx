- **`speed_chroma_cuda` leaked its CUDA module and stream on every init failure
  path — a regression, not unfinished work.** `cuModuleLoadData()` and
  `cuStreamCreate()` both jump to `fail_pop` on error, but that label (and
  `fail_after_pop`) only popped the context and called `free_cuda_buffers()`,
  which releases device/host buffers via `cuMemFree` / `cuMemFreeHost` and never
  touches `s->module` or `s->stream`. Any init failure after the module load
  leaked the module; any failure after the stream create leaked both.

  PR #1007 originally added the `cuModuleUnload` / `cuStreamDestroy` calls
  inline to both labels; PR #1029 — merged the same day as a descendant of
  #1007 — removed them again. The release is now factored into a single
  `release_cuda_module_and_stream()` helper called from both labels, so the two
  cannot drift apart a second time. It mirrors `close_fex_cuda()`'s order
  exactly: stream first, then module, each NULL-guarded, with no context push
  (the labels have already popped, and `close_fex_cuda()` releases them the same
  way). The sibling extractors `integer_ms_ssim_cuda.c`,
  `integer_psnr_hvs_cuda.c`, `ssimulacra2_cuda.c` and `speed_temporal_cuda.c`
  were already correct; `speed_chroma_cuda.c` was the only remaining file.
