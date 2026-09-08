- **SYCL translation units are const-correct.** The advisory `Tidy SYCL` lane
  carried 983 findings across 21 TUs, so it reported red on essentially any PR
  touching a SYCL kernel regardless of what that PR changed. This clears the
  `misc-const-correctness` share — 281 fixes across 19 files, bringing those
  files from 835 findings to 558. The fixes are annotations: they were applied
  with `clang-tidy --fix` restricted to that single check, and nothing else was
  edited. Verified on an Arc A380 with the full SYCL suite (156 pass); the only
  two failures are pre-existing `master` bugs with byte-identical signatures,
  already fixed in open PRs — the integer-ADM parity delta (#1369) and the SpEED
  AVX2 FMA contraction under icx (#1403). Remaining SYCL findings are the
  sign-comparison and widening classes, which need judgement per site rather
  than a mechanical fix. Progress toward the "one green master run" that
  [ADR-0217](docs/adr/0217-sycl-toolchain-cleanup.md) makes the condition for
  tightening the lane from advisory to required.
