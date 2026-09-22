- HISS-21 burn-down, `core/src/feature/` top level: the cleanup `goto`
  ladders are gone from `ciede`, `feature_collector`, `feature_dists`,
  `feature_lpips`, `float_moment`, `float_ms_ssim`, `float_psnr`,
  `float_ssim`, `motion` and `pu21` (29 HISS-01 findings), and the
  `float_ms_ssim` extractor's optional per-scale luminance / contrast /
  structure publication moved into its own helper so `extract` fits the
  60-LOC bound (1 HISS-04 finding). Scores do not move: the change is
  structural. Each former label ladder became one unwind helper that
  releases the same resources in the same order from every error exit,
  including the shallow early-error paths, and every arithmetic
  expression was moved whole rather than split across a helper boundary,
  so the compiler sees the same contraction opportunities it saw inline
  (ADR-1253). Verified with the full `meson test` suite and the Netflix
  CPU golden-data gate, assertions untouched.
- The ADR-1142 whole-tree clang-tidy ratchet baseline for the CPU lane
  (`scripts/ci/tidy-baseline-cpu.json`) is re-measured so it equals the tree
  again: 775 -> 751 warnings over 316 translation units. The measurement was
  taken on the lane's own toolchain (gcc-15 `Ubuntu 15.2.0-16ubuntu1`,
  clang-tidy 22.1.8) rather than on a workstation compiler, because a
  different libc's `assert` expansion makes `misc-static-assert` fire on
  `core/src/dict.cpp` and `core/src/feature/feature_collector.cpp` — files
  this branch never touched — and recording that would have raised their
  allowance. Every entry the re-measurement changes went down: the
  `float_ms_ssim` extractor split (3 -> 2), plus the reductions earlier
  commits on this branch already made in `framesync` (7 -> 0), its header and
  `log.h` (reserved include guards renamed), `test_framesync` (8 -> 0) and
  `test_pic_preallocation` (16 -> 10). No allowance was raised and no
  suppression was added.
