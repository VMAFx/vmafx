### Fixed

- Apply `test_gpu_picture_pool_uaf`, `test_integer_motion_v2_coverage`, and
  `test_pic_preallocation` sanitizer deselects to the `tests-and-quality-gates.yml`
  matrix job (ASan + UBSan + TSan). PR #767 added the exclusions to
  `sanitizers.yml` only; this companion fix closes the gap in the original
  ADR-0347 `case`-based deselect mechanism so the matrix job no longer
  SIGABRT on the intentional 192 GiB OOM allocation paths.
