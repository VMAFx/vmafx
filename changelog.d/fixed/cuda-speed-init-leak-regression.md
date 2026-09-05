- **`speed_chroma_cuda` and `speed_temporal_cuda` leaked their CUDA module and stream on init failure paths.**
  `cuModuleLoadData()` and `cuStreamCreate()` both jump to failure labels on error,
  but those labels (`fail_pop`, `fail_after_pop`, and `free_cpu`/`free_all`) only
  called `free_cuda_buffers` / `free_cuda_buffers_st`, which only release device
  and host memory and never touch `s->module` or `s->stream`.
  Any failure after module loading leaked the module, and any failure after stream
  creation leaked both. In `speed_chroma_cuda.c`, PR #1007 had originally added
  inline cleanup which was inadvertently removed during the PR #1029 merge;
  factoring the teardown into `release_cuda_module_and_stream` (and
  `release_cuda_module_and_stream_st` in `speed_temporal_cuda.c`) prevents future
  drift and unifies cleanup with `close_fex_cuda` / `close_fex_st`.
  `integer_ms_ssim_cuda.c` and `integer_psnr_hvs_cuda.c` failure exits are
  hardened to route through `close_fex_cuda` as well.
