
- **The cppcheck suppressions file no longer carries an "upstream" or
  "vendored" tier.** ADR-1142 removed those tiers and allows exactly four
  exemptions — generated files, third-party test fixtures, licence text and the
  Netflix golden assertions — so 31 entries were retired and the findings
  behind them fixed in the source rather than hidden. Among them: a
  `snprintf(key, sizeof(value), ...)` in `test_predict.c` that sized one buffer
  by another, three `%d` conversions applied to `unsigned` arguments, a
  type-punned `const int *` to `const float *` cast in the AVX2 float-ADM
  kernel replaced by `_mm256_castsi256_ps`, the `uint8_t *` to `double *` cast
  in `set_double` replaced by `std::memcpy`, and in vendored libsvm: 17
  uninitialised members, deleted copy operations on the four classes that own
  raw storage, `Solver::Solve` made virtual so `Solver_NU` overrides rather
  than hides it, and a real leak — `SVMModelParser` owned its `svm_model` with
  no destructor, so a parse that was never collected lost it. The Netflix
  golden assertions pass unchanged (99 passed, 2 skipped), and the whole-tree
  clang-tidy ratchet measures 1,048 against a 1,048 baseline.
