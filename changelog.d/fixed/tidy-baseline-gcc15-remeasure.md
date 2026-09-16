
- **The whole-tree clang-tidy baseline is re-measured against the lane's own
  toolchain.** Moving the lint lane from gcc-14 to gcc-15 changed the system
  headers clang-tidy parses and therefore the warning counts, but the baseline
  was not re-measured in the same change: it claimed 1,229 warnings over 292
  translation units against a measured 1,059 over 306. Twenty-five files had
  improved without the baseline tightening and one untouched file had drifted
  upward. The five genuine regressions are fixed in the code rather than
  baselined — the two ssimulacra2 SIMD files by extracting the per-pixel
  edge-diff accumulation both their vector body and their scalar tail carried,
  which also removes the duplication ADR-1208 exists to prevent.
