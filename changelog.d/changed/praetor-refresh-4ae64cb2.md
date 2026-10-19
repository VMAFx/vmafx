- **Standards gate on praetor `4ae64cb2`.** The gate pins praetor `4ae64cb27`,
  which reads `clang_tidy.data_paths`: `.standards.yaml` declares
  `scripts/ci/exact_twins.d` there, so the 24 parity-gate fragments named
  `*.hip` (ADR-1428) leave the clang-tidy exception list. Lint exceptions in
  `.config/lint-exceptions.d/` can carry a `category` and an `entered` date,
  which `scripts/ci/praetor_tidy_coverage.py --write` renders; the release
  workflow's token fallback is listed as a category C `release-token`
  exception until its rework is decided.
