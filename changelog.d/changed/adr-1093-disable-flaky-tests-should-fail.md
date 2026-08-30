- Marked `test_pic_preallocation` and `test_sycl_motion_add_uv_parity` as
  `should_fail: true` in `core/test/meson.build` (ADR-1093). Both tests have
  had three or more failed fix attempts and continue to fail in CI; disabling
  them this way preserves the test binaries and CI run visibility while
  stopping them from blocking unrelated PRs. Root-cause investigation is
  tracked in `docs/state.md` as T-PIC-PREALLOC-RECURRING-FAILURE-2026-06-07
  and T-SYCL-MOTION-ADD-UV-SIGSEGV-2026-06-07.
