### Fixed

- **Concurrent SSIM SIMD dispatch install no longer races on shared
  globals** — A TSan audit (worktree `fix/tsan-race-audit`, master
  `bbcaa8d127`) of the libvmaf thread pool with `--threads 16` and
  both `float_ssim` + `float_ms_ssim` enabled surfaced ten data-race
  warnings on four process-wide dispatch function pointers
  (`g_ssim_precompute`, `g_ssim_variance`, `g_ssim_accumulate`,
  `g_iqa_convolve`). The per-extractor `init()` callback for each
  worker thread was racing to install the same ISA-best dispatch
  table. Gate the install behind a single process-wide
  `pthread_once_t` owned by `core/src/feature/iqa/ssim_tools.c`,
  shared between `float_ssim.c` and `float_ms_ssim.c` via the new
  `iqa_ssim_install_dispatch_once()` helper. Re-running the stress
  test shows zero TSan warnings; bit-for-bit identical scores
  (`pooled vmaf.mean = 76.66783` on the src01_hrc00/src01_hrc01
  reference pair). All 63 `meson test -C build-tsan` cases pass
  clean under `-Db_sanitize=thread`. See
  [ADR-0871](../docs/adr/0871-ssim-dispatch-pthread-once.md).
