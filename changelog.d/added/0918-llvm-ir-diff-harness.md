- **LLVM IR diff harness** (`scripts/perf/check-ir-diff.sh`,
  `make ir-diff` / `make ir-diff-update`): opt-in build-time gate
  that compiles fork-added SIMD `.c` sources with
  `clang -O2 -mavx2 -mfma -emit-llvm -S`, extracts per-function IR,
  normalises non-semantic noise (debug metadata, attribute IDs,
  source paths), and diffs against snapshots under
  `testdata/ir-snapshots/`. Seeded with 8 bit-exact-required
  functions across `psnr_hvs_avx2.c`, `ms_ssim_decimate_avx2.c`,
  and `ssimulacra2_avx2.c`. Designed to catch compiler-induced
  FMA / FP-contract regressions of the kind that took two review
  rounds each on PR #339 and PR #382 — at build time, naming the
  affected function and printing an FMA-count delta, rather than as
  a 4-ULP score drift three minutes later. NOT a default CI gate
  (would add a clang re-compile per SIMD file to every PR);
  invoked manually after touching SIMD sources or bumping the
  clang version in `dev/Containerfile` / GitHub Actions runners.
  ADR-0918.
