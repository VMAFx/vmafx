- New `nvcc_threads` build option (default `4`) passes `--threads` to the
  per-kernel CUDA fatbin compiles. nvcc parallelises across the six
  gencode architectures, so this shortens the critical-path kernel:
  `adm_cm.cu` 8.5 s → 2.1 s, serial wall 41.9 s → 14.2 s. Verified
  byte-identical: all **22/22** fatbins compare equal (`cmp`) between
  `--threads 4` and `--threads 1`, so it cannot move a score. ninja
  already builds the 22 fatbin targets concurrently, so the effective
  thread count is this value times the ninja job count — lower it on a
  small runner. See ADR-1224.
