- **Vendored libsvm 3.24 audit closes one residual parser oob in `svm.cpp`
  and ratifies the existing fork patch set.** The fork-local
  SAN-MODEL-MALLOC-OOB hardening (introduced 2026-05-09) bounded the
  `nr_class` / `total_sv` axis sizes but did not pre-flight per-row
  `Malloc(...)` calls for the case where the model file's `nr_class`
  row appears after a row that derives its allocation size from it
  (`rho`, `label`, `probA`, `probB`, `nr_sv`). A crafted model could
  therefore allocate a zero-size buffer and let downstream
  `svm_predict_values` / `svm_predict_probability` dereference it as an
  array (UB; not exploitable under glibc, which returns a non-NULL
  one-byte allocation for `malloc(0)`, but a SIGSEGV risk on stricter
  allocators). Added a `model->nr_class > 0` precondition assert to
  every affected row inside `parse_header()`. Lands a 9-case fork-local
  regression suite at `core/test/test_svm_parser.c` (suite `fast`)
  covering the eight rejection paths plus the legacy unknown-`svm_type`
  case. The upstream-version audit confirms no CVE-grade fix from
  libsvm 3.25 – 3.36 needs backport; the pin remains 3.24 plus the
  three fork patches (thread-locale isolation, JSON entry point,
  MALLOC-OOB hardening). See ADR-0889 and
  `docs/research/0889-libsvm-vendored-audit-2026-05-30.md`.
