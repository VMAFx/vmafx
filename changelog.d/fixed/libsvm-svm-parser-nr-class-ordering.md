- **`core/src/svm.cpp` parser now rejects header rows that depend on
  `nr_class` if they appear before the `nr_class` row itself.** Five
  affected rows (`rho`, `label`, `probA`, `probB`, `nr_sv`) gain a
  one-line `exceptAssert(model->nr_class > 0, ...)` precondition before
  the `Malloc(...)` call. Closes a residual gap in the
  SAN-MODEL-MALLOC-OOB hardening introduced 2026-05-09. See ADR-0889.
