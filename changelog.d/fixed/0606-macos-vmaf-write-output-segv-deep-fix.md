Fix macOS SIGSEGV in `test_output` and `test_public_api_score` (ADR-0606, PR #1403 follow-up):
seven `i > capacity` off-by-one bounds checks corrected to `i >= capacity`; fps computation
guarded against `0.0/0.0`; JSON pool-score and frame-list comma tracking corrected.
