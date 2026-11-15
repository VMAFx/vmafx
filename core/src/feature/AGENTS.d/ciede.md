---
paths:
  - core/src/feature/ciede.c
  - core/test/test_ciede.c
invariant: CIEDE chroma-upsample subsample flags diverge from upstream to ensure correctness.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CIEDE Chroma-Upsample Subsample Flags Invariant

- **CIEDE chroma-upsample subsample flags (FORK DIVERGES FROM
  UPSTREAM, 2026-06-27)**: `ciede.c` `scale_chroma_planes` /
  `scale_chroma_planes_hbd` must key *horizontal* sample-index
  divisor off `ss_hor` and *vertical* row advance off `ss_ver`.
  **Upstream Netflix carries these two flags transposed** (horizontal
  off `ss_ver`, vertical off `ss_hor`) — that bug heap-OOB-reads and
  mis-scores YUV422P (half-width / full-height chroma). YUV420P is
  no-op (both flags set) and YUV444P never calls function, so
  Netflix golden CIEDE2000 pair (420P) cannot detect regression
  here. On rebase, do NOT let upstream sync revert flags back to
  transposed form. Guarded by `test_ciede_scale_chroma_422_8b` and
  `test_ciede_scale_chroma_422_16b` in `core/test/test_ciede.c` (both
  fail against upstream form).
