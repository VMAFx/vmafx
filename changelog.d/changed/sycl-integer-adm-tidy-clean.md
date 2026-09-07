- **`integer_adm_sycl.cpp` is clang-tidy clean.** The file carried 46 findings —
  28 `misc-const-correctness`, 9 `modernize-use-integer-sign-comparison`, 9
  `bugprone-implicit-widening-of-multiplication-result` — so the advisory
  `Tidy SYCL` lane went red on every PR that touched it, regardless of what the
  PR changed. The `const` additions are annotations. The signed/unsigned
  comparisons dropped their `(int)` casts for `std::cmp_greater_equal`, which
  compares mathematically instead of silencing the mismatch by truncation
  (verified to compile in SYCL device code under icpx 2026.0). The widening
  fixes compute launch extents and the `3 * num_rows * WG_SIZE` global size in
  `size_t`, so the product cannot overflow before it is widened. No behaviour
  change: on an Arc A380 the `test_sycl_adm_parity` scores are byte-identical
  before and after (`cpu=0.58175555 sycl=0.58191226` in both). This is a step
  toward the "one green master run" that [ADR-0217](docs/adr/0217-sycl-toolchain-cleanup.md)
  makes the condition for tightening the lane from advisory to required.
