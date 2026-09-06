- **Strict input and path validation in extract_ugc_features subprocess (2026-09-04)** —
  `ai/scripts/extract_ugc_features.py::_run_vmaf` now performs strict input validation
  on video dimensions (`w > 0`, `h > 0`), thread counts (`n_threads > 0`), input/output
  paths (rejection of empty/dot paths and null bytes), and `VMAF_TINY_AI_SCRATCH`
  environment variable values (requiring absolute paths, non-empty, and verifying that
  the generated temporary output file stays within the resolved scratch directory).
  Replaces false-positive dismissal for Semgrep rule
  `python.lang.security.audit.dangerous-subprocess-use-tainted-env-args` (Alert 372)
  with active defensive validation. Regression tests added in
  `ai/tests/test_extract_ugc_features.py`.
