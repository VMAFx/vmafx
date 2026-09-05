- **docs(state):** Removed the stale `T-CUDA-INIT-SUBMIT-LEAKS-2026-06-19`
  duplicate from the Open-bugs section of `docs/state.md`; the bug is closed
  and its Recently-closed row is now the single row for that id. Re-audited
  the CUDA sources on `origin/master` (not the ledger prose) and extended the
  closed row with the evidence: `speed_chroma_cuda.c` / `speed_temporal_cuda.c`
  release module + stream from every failure label via
  `release_cuda_module_and_stream[_st]()` (commits e36a1ef2f, 0a8faa2e9), and
  `integer_ms_ssim_cuda.c`, `integer_psnr_hvs_cuda.c` and `ssimulacra2_cuda.c`
  funnel their init/alloc failure paths through `close_fex_cuda()`
  (commit 86f5a02655). One residual out-of-scope nit is recorded in the row.
