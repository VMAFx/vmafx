- **CI test and scan failures no longer become green results.** Python tox
  coverage errors, CPU coverage pytest failures, nightly Netflix benchmark
  failures, advisory Semgrep registry failures, and sanitizer test-discovery
  errors now preserve their real exit status. Coverage artifacts are still
  uploaded after a pytest failure before the job reasserts that failure. A
  permanently disabled, misleading cross-backend placeholder job was removed,
  and the previously reverted required GPU coverage status was restored.
