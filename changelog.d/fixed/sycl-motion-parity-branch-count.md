- `core/test/test_sycl_motion_add_uv_parity.c` +
  `core/test/test_sycl_motion3_parity.c`: resolve open ratchet defect
  `T-SYCL-RATCHET-TEST-BRANCH-COUNT-2026-09-22`.
  - Refactored `run_sycl_pass_add_uv()` and `run_sycl_pass_y_only()` in
    `test_sycl_motion_add_uv_parity.c` into branch-bounded phase helpers
    (`setup_*` and `feed_*_frames`) returning `mu_message_t` and propagated
    via `mu_assert_msg()`. Each helper and caller now carries <= 9 branches,
    well under clang-tidy's `readability-function-size` BranchThreshold of 15,
    bringing warnings from 2 to 0 without increasing or modifying the zero baseline in
    `scripts/ci/tidy-baseline-sycl.json`.
  - Preserved every assertion message byte-for-byte, exact numeric comparison,
    frame order, feature configuration, and context cleanup path.
  - Refactored `run_sycl_checkerboard()` in `test_sycl_motion3_parity.c` using the
    same phase helper pattern (`setup_sycl_checkerboard` and
    `collect_sycl_checkerboard_scores`), reducing branches from 24 to 12 and
    bringing warnings from 1 to 0.
  - Added regression contract test `SyclMotionAddUvParityTidyContract` in
    `scripts/ci/tests/test_tidy_ratchet.py` proving that the SYCL ratchet lane
    measures `test_sycl_motion_add_uv_parity.c` at zero baseline and rejects
    the unrefactored diagnostic pattern.
  - Added SYCL parity test phase helper pattern invariant note to
    `core/test/AGENTS.md`.
